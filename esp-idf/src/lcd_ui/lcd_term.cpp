/**
 * lcd_term.cpp — libvterm-backed terminal widget (see lcd_term.h).
 *
 * libvterm owns the VT state machine + the rows×cols live screen. We render a
 * window over [scrollback ++ live screen] into per-row label runs. Lines that
 * scroll off the top are kept (sb_pushline) in a PSRAM ring so you can drag to
 * scroll back; any keypress (and new output while at the bottom) snaps back to
 * the live screen.
 *
 * Colour: each visible row is painted as a row of labels, one per run of cells
 * sharing (fg, bg) — SGR colours from the application (ls, logs, prompts)
 * render, including backgrounds and reverse video. LVGL's label "recolor" was
 * rejected for this: its '#' escape is broken in 9.5 (lv_text_is_cmd's escape
 * branch is dead code), and terminal text is full of literal '#'. Run labels
 * sidestep that and get backgrounds, which recolor can't do at all. Default-
 * coloured text uses the widget fg passed at create; scrollback lines store
 * their runs in a compact per-line string (0x1F marker + fg/bg + text per run)
 * so colour survives scrolling without a vector-per-line heap footprint.
 */
#include "lcd_term.h"
#include "lcd.h"            /* LCD_KEY_CTRL */

extern "C" {
#include "vterm.h"
}

#include "esp_heap_caps.h"
#include <cstring>
#include <deque>
#include <string>
#include <vector>

/* libvterm's screen is ~130 KB at 64x26 (two cell buffers) — keep it off the
 * scarce internal heap. libvterm requires malloc to return zeroed memory. */
namespace {
void* vtMalloc(size_t size, void*) { return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM); }
void  vtFree(void* ptr, void*)     { heap_caps_free(ptr); }
VTermAllocatorFunctions VT_ALLOC = { vtMalloc, vtFree };

constexpr size_t SB_CAP = 600;     /* scrollback lines retained */

/* "No colour": use the widget default fg / transparent bg. */
constexpr uint32_t TERM_DEFAULT = 0xFFFFFFFFu;

void utf8Append(std::string& s, uint32_t cp) {
    if (cp < 0x80) s.push_back((char)cp);
    else if (cp < 0x800) {
        s.push_back((char)(0xC0 | (cp >> 6)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back((char)(0xE0 | (cp >> 12)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        s.push_back((char)(0xF0 | (cp >> 18)));
        s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

/* One run of consecutive cells sharing (fg, bg). `cols` is the cell count
 * (label x/width are cols * charW; byte length differs under UTF-8). */
struct Run {
    uint32_t    fg = TERM_DEFAULT;
    uint32_t    bg = TERM_DEFAULT;
    uint16_t    cols = 0;
    std::string text;
};

/* A colour that no real run carries (fg/bg are 24-bit or TERM_DEFAULT), used to
 * force the first style application on a freshly created slot. */
constexpr uint32_t COLOR_UNSET = 0xFFFFFFFEu;

/* A pooled run label plus the state last applied to it. paintRow only touches an
 * LVGL property (which invalidates the label's area) when it actually changed —
 * so typing a character repaints only the run that grew, not the prompt or the
 * rest of the row. */
struct LabelSlot {
    lv_obj_t*   obj = nullptr;
    uint32_t    fg = COLOR_UNSET, bg = COLOR_UNSET;
    int         col = -1, cols = -1;
    std::string text;
    bool        hidden = true;
};
}  // namespace

struct lcd_term_t {
    VTerm*       vt   = nullptr;
    VTermScreen* scr  = nullptr;
    lv_obj_t*    cont = nullptr;          /* grid container = focus + scroll target */
    lv_obj_t*    cursor = nullptr;        /* cursor block, above the labels */
    const lv_font_t* font = nullptr;
    lv_color_t   fg{};
    uint32_t     fgRgb = 0xC8C8C8;        /* fg as 24-bit, for reverse-video swaps */
    int rows = 0, cols = 0, charW = 5, rowH = 8;
    lcd_term_output_cb out = nullptr;
    void* user = nullptr;
    std::vector<std::vector<LabelSlot>> rowLbl;  /* per visible row: pooled run labels + cache */
    std::deque<std::string> sb;           /* scrollback: encoded run lines (see sbEncode) */
    int      top  = 0;                    /* virtual row of the topmost visible row */
    bool     follow = true;               /* stick to the live screen (bottom)       */
    VTermPos cur{};                       /* cursor cell (live-screen coords)         */
    int      curVisible = 1;
    /* Damage accounting for the current feed batch (all in visible-row coords,
     * which equal live-screen rows while following). dmg[Lo,Hi) is the damaged
     * row span; dmgAll forces a full repaint (a scroll shifts every row);
     * needCursor repositions the cursor overlay without repainting any row. */
    int      dmgLo = 0, dmgHi = 0;
    bool     dmgAll = false;
    bool     needCursor = false;
    /* drag-to-scroll */
    int      dragStartY = 0, dragStartTop = 0;
    bool     dragging = false;
};

namespace {

/* Resolve a cell's colours to 24-bit (or TERM_DEFAULT), applying reverse
 * video. Defaults resolve against the widget palette (fg) / black (bg) when a
 * reverse swap forces them concrete. */
void cellColors(lcd_term_t* t, const VTermScreenCell& cell, uint32_t& fg, uint32_t& bg) {
    VTermColor f = cell.fg, b = cell.bg;
    if (VTERM_COLOR_IS_DEFAULT_FG(&f)) fg = TERM_DEFAULT;
    else {
        vterm_screen_convert_color_to_rgb(t->scr, &f);
        fg = ((uint32_t)f.rgb.red << 16) | ((uint32_t)f.rgb.green << 8) | f.rgb.blue;
    }
    if (VTERM_COLOR_IS_DEFAULT_BG(&b)) bg = TERM_DEFAULT;
    else {
        vterm_screen_convert_color_to_rgb(t->scr, &b);
        bg = ((uint32_t)b.rgb.red << 16) | ((uint32_t)b.rgb.green << 8) | b.rgb.blue;
    }
    if (cell.attrs.reverse) {
        uint32_t nf = (bg == TERM_DEFAULT) ? 0x000000u : bg;
        uint32_t nb = (fg == TERM_DEFAULT) ? t->fgRgb  : fg;
        fg = nf; bg = nb;
    }
}

void runAppendCell(lcd_term_t* t, std::vector<Run>& runs, const VTermScreenCell& cell, int width) {
    uint32_t fg, bg;
    cellColors(t, cell, fg, bg);
    if (runs.empty() || runs.back().fg != fg || runs.back().bg != bg) {
        runs.push_back(Run{});
        runs.back().fg = fg;
        runs.back().bg = bg;
    }
    Run& r = runs.back();
    uint32_t ch = cell.chars[0];
    if (ch == 0 || ch == (uint32_t)-1) r.text.push_back(' ');
    else                               utf8Append(r.text, ch);
    for (int k = 1; k < width; k++) r.text.push_back(' ');   /* wide-cell padding */
    r.cols = (uint16_t)(r.cols + width);
}

/* Runs of one live-screen row, exactly `cols` columns wide (blank/continuation
 * cells → space) so the grid stays aligned. */
std::vector<Run> screenRowRuns(lcd_term_t* t, int sr) {
    std::vector<Run> runs;
    for (int c = 0; c < t->cols; ) {
        VTermScreenCell cell;
        VTermPos p; p.row = sr; p.col = c;
        if (vterm_screen_get_cell(t->scr, p, &cell)) {
            int w = cell.width < 1 ? 1 : cell.width;
            runAppendCell(t, runs, cell, w);
            c += w;
        } else {
            VTermScreenCell blank{};   /* zeroed colours read as RGB black, not default */
            blank.fg.type = VTERM_COLOR_DEFAULT_FG;
            blank.bg.type = VTERM_COLOR_DEFAULT_BG;
            runAppendCell(t, runs, blank, 1);
            c++;
        }
    }
    return runs;
}

/* Scrollback line codec: per run, 0x1F marker + fg + bg + cols (raw LE) + text.
 * One std::string per line keeps the ring's heap profile close to plain text.
 * 0x1F (unit separator) can't occur in cell text (control chars never reach
 * the screen as glyphs). */
std::string sbEncode(const std::vector<Run>& runs) {
    std::string s;
    for (const Run& r : runs) {
        s.push_back('\x1f');
        char hdr[10];
        memcpy(hdr,     &r.fg,   4);
        memcpy(hdr + 4, &r.bg,   4);
        memcpy(hdr + 8, &r.cols, 2);
        s.append(hdr, 10);
        s.append(r.text);
    }
    return s;
}

std::vector<Run> sbDecode(const std::string& s) {
    std::vector<Run> runs;
    size_t i = 0;
    while (i < s.size() && s[i] == '\x1f' && i + 11 <= s.size()) {
        Run r;
        memcpy(&r.fg,   s.data() + i + 1, 4);
        memcpy(&r.bg,   s.data() + i + 5, 4);
        memcpy(&r.cols, s.data() + i + 9, 2);
        i += 11;
        size_t end = s.find('\x1f', i);
        if (end == std::string::npos) end = s.size();
        r.text.assign(s, i, end - i);
        i = end;
        runs.push_back(std::move(r));
    }
    return runs;
}

int maxTop(lcd_term_t* t) { return (int)t->sb.size(); }   /* bottom = live screen at window top 0 */

/* Paint one viewport row from its runs, reusing that row's label pool. Each LVGL
 * setter invalidates the label's area, so we apply one only when its cached
 * value actually changed — a keystroke then repaints just the run that grew, not
 * the whole row. Blank runs with the default (transparent) background get no
 * label at all: the container's black shows through, and skipping them keeps the
 * full-width trailing filler from repainting on every keystroke. x advances by
 * run cell counts. */
void paintRow(lcd_term_t* t, int r, const std::vector<Run>& runs) {
    auto& pool = t->rowLbl[r];
    size_t li = 0;
    int col = 0;
    for (const Run& run : runs) {
        if (run.cols == 0) continue;
        if (run.bg == TERM_DEFAULT && run.text.find_first_not_of(' ') == std::string::npos) {
            col += run.cols;                    /* blank filler — no label needed */
            continue;
        }
        if (li >= pool.size()) pool.push_back(LabelSlot{});
        LabelSlot& sl = pool[li++];
        if (!sl.obj) {
            lv_obj_t* l = lv_label_create(t->cont);
            lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_style_text_font(l, t->font, 0);
            lv_obj_set_style_text_line_space(l, 0, 0);
            lv_obj_set_style_text_letter_space(l, 0, 0);
            sl.obj = l;
            /* Labels are created lazily, i.e. after the cursor — keep the
             * cursor block composited above the run labels. */
            if (t->cursor) lv_obj_move_foreground(t->cursor);
        }
        lv_obj_t* l = sl.obj;
        if (sl.hidden)                { lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN); sl.hidden = false; }
        if (sl.col != col || sl.cols != run.cols) {
            lv_obj_set_pos(l, col * t->charW, r * t->rowH);
            lv_obj_set_size(l, run.cols * t->charW, t->rowH);
            sl.col = col; sl.cols = run.cols;
        }
        if (sl.fg != run.fg) {
            lv_obj_set_style_text_color(l, run.fg == TERM_DEFAULT ? t->fg : lv_color_hex(run.fg), 0);
            sl.fg = run.fg;
        }
        if (sl.bg != run.bg) {
            if (run.bg == TERM_DEFAULT) {
                lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
            } else {
                lv_obj_set_style_bg_color(l, lv_color_hex(run.bg), 0);
                lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
            }
            sl.bg = run.bg;
        }
        if (sl.text != run.text) { lv_label_set_text(l, run.text.c_str()); sl.text = run.text; }
        col += run.cols;
    }
    for (; li < pool.size(); li++)
        if (pool[li].obj && !pool[li].hidden) { lv_obj_add_flag(pool[li].obj, LV_OBJ_FLAG_HIDDEN); pool[li].hidden = true; }
}

/* Position the cursor overlay (a separate object composited above the run
 * labels), or hide it. Cheap — no row repaint, so cursor-only moves take this
 * path. Cursor shows only on the live screen (when following). */
void placeCursor(lcd_term_t* t) {
    bool curOnScreen = t->follow && t->curVisible && t->cur.row >= 0 && t->cur.row < t->rows;
    if (!curOnScreen) { lv_obj_add_flag(t->cursor, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(t->cursor, LV_OBJ_FLAG_HIDDEN);
    const int nsb = (int)t->sb.size();
    int winRow = (nsb - t->top) + t->cur.row;        /* live row 0 sits at window row (nsb-top) */
    lv_obj_set_pos(t->cursor, t->cur.col * t->charW, winRow * t->rowH);
}

/* Repaint visible rows [lo, hi) from the window [top, top+rows) over the
 * virtual buffer (scrollback then live screen), then place the cursor. Painting
 * only the damaged span is the difference between a one-row keystroke update and
 * a whole-viewport redraw (each paintRow invalidates its labels' areas). */
void renderRows(lcd_term_t* t, int lo, int hi) {
    if (t->follow) t->top = maxTop(t);
    if (t->top < 0) t->top = 0;
    if (t->top > maxTop(t)) t->top = maxTop(t);
    if (lo < 0) lo = 0;
    if (hi > t->rows) hi = t->rows;
    const int nsb = (int)t->sb.size();
    for (int r = lo; r < hi; r++) {
        int v = t->top + r;
        if (v < nsb)                 paintRow(t, r, sbDecode(t->sb[v]));
        else if (v < nsb + t->rows)  paintRow(t, r, screenRowRuns(t, v - nsb));
        else                         paintRow(t, r, {});
    }
    placeCursor(t);
}

/* Repaint every visible row (scroll, scroll-back navigation, first paint). */
void renderView(lcd_term_t* t) { renderRows(t, 0, t->rows); }

/* ---- libvterm screen callbacks (fire during vterm_input_write) ---- */

int cbDamage(VTermRect rect, void* u) {
    auto* t = (lcd_term_t*)u;
    if (rect.start_row < t->dmgLo) t->dmgLo = rect.start_row;   /* end_row is exclusive */
    if (rect.end_row   > t->dmgHi) t->dmgHi = rect.end_row;
    return 1;
}
int cbMoveCursor(VTermPos pos, VTermPos, int visible, void* u) {
    /* Cursor is an overlay object — moving it needs no row repaint. */
    auto* t = (lcd_term_t*)u; t->cur = pos; t->curVisible = visible; t->needCursor = true; return 1;
}
int cbSetProp(VTermProp prop, VTermValue* val, void* u) {
    auto* t = (lcd_term_t*)u;
    if (prop == VTERM_PROP_CURSORVISIBLE) { t->curVisible = val->boolean; t->needCursor = true; }
    return 1;
}
/* A line scrolled off the top of the live screen — keep it for scrollback. */
int cbSbPushline(int cols, const VTermScreenCell* cells, void* u) {
    auto* t = (lcd_term_t*)u;
    std::vector<Run> runs;
    for (int c = 0; c < cols; c++) {
        int w = cells[c].width < 1 ? 1 : cells[c].width;
        runAppendCell(t, runs, cells[c], w);
        c += w - 1;
    }
    /* Trim trailing blanks to save memory — but only where the background is
     * default (a coloured-bg run of spaces is visible ink). */
    while (!runs.empty()) {
        Run& last = runs.back();
        if (last.bg != TERM_DEFAULT) break;
        size_t end = last.text.find_last_not_of(' ');
        if (end == std::string::npos) { runs.pop_back(); continue; }
        if (end + 1 < last.text.size()) {
            last.cols = (uint16_t)(last.cols - (last.text.size() - (end + 1)));
            last.text.erase(end + 1);
        }
        break;
    }
    t->sb.push_back(sbEncode(runs));
    if (t->sb.size() > SB_CAP) {
        t->sb.pop_front();
        if (!t->follow && t->top > 0) t->top--;   /* keep the scrolled-back view stable */
    }
    t->dmgAll = true;   /* a line scrolled off → every visible row shifts up */
    return 1;
}

/* Zero-initialised (static storage), so every callback we don't set stays NULL.
 * Filled in once in lcdTermCreate; libvterm stores this pointer, so it must
 * outlive the screen — hence static. */
VTermScreenCallbacks SCREEN_CBS;

void cbOutput(const char* s, size_t len, void* u) {
    auto* t = (lcd_term_t*)u;
    if (t->out) t->out(s, len, t->user);
}

/* Drag the terminal body up/down to scroll back through history; a small tap
 * doesn't move and still focuses (the CLICKED handler in the owner). */
void onPress(lv_event_t* e) {
    auto* t = (lcd_term_t*)lv_event_get_user_data(e);
    lv_indev_t* ind = lv_indev_active();
    if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    t->dragStartY = p.y; t->dragStartTop = t->top; t->dragging = true;
}
void onPressing(lv_event_t* e) {
    auto* t = (lcd_term_t*)lv_event_get_user_data(e);
    if (!t->dragging) return;
    lv_indev_t* ind = lv_indev_active();
    if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    int delta = (p.y - t->dragStartY) / t->rowH;     /* drag down → reveal older */
    int nt = t->dragStartTop - delta;
    if (nt < 0) nt = 0;
    if (nt > maxTop(t)) nt = maxTop(t);
    if (nt != t->top) {
        t->top = nt;
        t->follow = (t->top >= maxTop(t));
        renderView(t);
    }
}

}  // namespace

lcd_term_t* lcdTermCreate(lv_obj_t* parent, int32_t w, int32_t h,
                          const lv_font_t* font, lv_color_t fg,
                          lcd_term_output_cb onOutput, void* user) {
    auto* t = new lcd_term_t();
    t->font = font; t->fg = fg; t->out = onOutput; t->user = user;
    t->fgRgb = lv_color_to_u32(fg) & 0xFFFFFF;
    t->rowH  = lv_font_get_line_height(font);  if (t->rowH  < 1) t->rowH  = 8;
    t->charW = lv_font_get_glyph_width(font, 'M', 0); if (t->charW < 1) t->charW = 5;
    t->cols = w / t->charW; if (t->cols < 1) t->cols = 1;
    t->rows = h / t->rowH;  if (t->rows < 1) t->rows = 1;
    t->top  = 0;

    t->cont = lv_obj_create(parent);
    lv_obj_remove_style_all(t->cont);
    lv_obj_set_size(t->cont, w, h);
    lv_obj_set_style_bg_color(t->cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(t->cont, LV_OPA_COVER, 0);
    lv_obj_remove_flag(t->cont, LV_OBJ_FLAG_SCROLLABLE);   /* we scroll the window ourselves */
    lv_obj_add_flag(t->cont, LV_OBJ_FLAG_CLICKABLE);       /* receive press/click for drag + focus */
    lv_obj_add_event_cb(t->cont, onPress,    LV_EVENT_PRESSED,  t);
    lv_obj_add_event_cb(t->cont, onPressing, LV_EVENT_PRESSING, t);

    /* Per-row label pools start empty; paintRow creates labels as runs need
     * them (a monochrome row costs one label, same as the old design). */
    t->rowLbl.resize(t->rows);

    t->cursor = lv_obj_create(t->cont);
    lv_obj_remove_style_all(t->cursor);
    lv_obj_set_size(t->cursor, t->charW, t->rowH);
    lv_obj_set_style_bg_color(t->cursor, fg, 0);
    lv_obj_set_style_bg_opa(t->cursor, LV_OPA_50, 0);
    lv_obj_remove_flag(t->cursor, LV_OBJ_FLAG_CLICKABLE);

    SCREEN_CBS.damage      = cbDamage;
    SCREEN_CBS.movecursor  = cbMoveCursor;
    SCREEN_CBS.settermprop = cbSetProp;
    SCREEN_CBS.sb_pushline  = cbSbPushline;

    t->vt = vterm_new_with_allocator(t->rows, t->cols, &VT_ALLOC, nullptr);
    vterm_set_utf8(t->vt, 1);
    vterm_output_set_callback(t->vt, cbOutput, t);
    t->scr = vterm_obtain_screen(t->vt);
    vterm_screen_set_callbacks(t->scr, &SCREEN_CBS, t);
    vterm_screen_set_damage_merge(t->scr, VTERM_DAMAGE_SCROLL);
    vterm_screen_reset(t->scr, 1);
    renderView(t);
    return t;
}

void lcdTermFeed(lcd_term_t* t, const char* data, size_t len) {
    if (!t || !t->vt || !data || len == 0) return;
    /* Start clean damage accounting; callbacks below accumulate into it. */
    t->dmgLo = t->rows; t->dmgHi = 0; t->dmgAll = false; t->needCursor = false;
    vterm_input_write(t->vt, data, len);
    vterm_screen_flush_damage(t->scr);   /* deliver merged damage → our callbacks */
    if (!t->follow) return;              /* scrolled-back view stays put until you return */
    if (t->dmgAll)                renderView(t);
    else if (t->dmgHi > t->dmgLo) renderRows(t, t->dmgLo, t->dmgHi);
    else if (t->needCursor)       placeCursor(t);
}

void lcdTermKey(lcd_term_t* t, uint32_t k) {
    if (!t || !t->vt) return;
    if (!t->follow) { t->follow = true; renderView(t); }   /* typing returns to the live screen */
    if (k & LCD_KEY_CTRL) {                                 /* Ctrl-<letter> from the keyboard driver */
        vterm_keyboard_unichar(t->vt, k & 0xFF, VTERM_MOD_CTRL);
        return;
    }
    switch (k) {
        case LV_KEY_ENTER:     vterm_keyboard_key(t->vt, VTERM_KEY_ENTER,     VTERM_MOD_NONE); break;
        case LV_KEY_BACKSPACE: vterm_keyboard_key(t->vt, VTERM_KEY_BACKSPACE, VTERM_MOD_NONE); break;
        case LV_KEY_ESC:       vterm_keyboard_key(t->vt, VTERM_KEY_ESCAPE,    VTERM_MOD_NONE); break;
        case LV_KEY_UP:        vterm_keyboard_key(t->vt, VTERM_KEY_UP,        VTERM_MOD_NONE); break;
        case LV_KEY_DOWN:      vterm_keyboard_key(t->vt, VTERM_KEY_DOWN,      VTERM_MOD_NONE); break;
        case LV_KEY_LEFT:      vterm_keyboard_key(t->vt, VTERM_KEY_LEFT,      VTERM_MOD_NONE); break;
        case LV_KEY_RIGHT:     vterm_keyboard_key(t->vt, VTERM_KEY_RIGHT,     VTERM_MOD_NONE); break;
        default:
            if (k >= 0x20 && k < 0x7F) vterm_keyboard_unichar(t->vt, k, VTERM_MOD_NONE);
            break;
    }
}

void lcdTermSize(lcd_term_t* t, int* rows, int* cols) {
    if (rows) *rows = t ? t->rows : 0;
    if (cols) *cols = t ? t->cols : 0;
}

lv_obj_t* lcdTermObj(lcd_term_t* t) { return t ? t->cont : nullptr; }

void lcdTermDestroy(lcd_term_t* t) {
    if (!t) return;
    if (t->vt) vterm_free(t->vt);
    delete t;                 /* LVGL objects are freed with the parent layer */
}
