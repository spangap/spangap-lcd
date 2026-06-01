/**
 * lcd_term.cpp — libvterm-backed terminal widget (see lcd_term.h).
 *
 * libvterm owns the VT state machine + the rows×cols live screen. We render a
 * window over [scrollback ++ live screen] into one LVGL label per visible row.
 * Lines that scroll off the top are kept (sb_pushline) in a PSRAM ring so you
 * can drag to scroll back; any keypress (and new output while at the bottom)
 * snaps back to the live screen. Monochrome for now: colour/attrs are read off
 * the cells but not yet styled (a contained follow-up).
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
}  // namespace

struct lcd_term_t {
    VTerm*       vt   = nullptr;
    VTermScreen* scr  = nullptr;
    lv_obj_t*    cont = nullptr;          /* grid container = focus + scroll target */
    lv_obj_t*    cursor = nullptr;        /* cursor block, above the labels */
    const lv_font_t* font = nullptr;
    lv_color_t   fg{};
    int rows = 0, cols = 0, charW = 5, rowH = 8;
    lcd_term_output_cb out = nullptr;
    void* user = nullptr;
    std::vector<lv_obj_t*> rowLbl;        /* one label per visible viewport row */
    std::deque<std::string> sb;           /* scrollback: lines scrolled off the top */
    int      top  = 0;                    /* virtual row of the topmost visible row */
    bool     follow = true;               /* stick to the live screen (bottom)       */
    VTermPos cur{};                       /* cursor cell (live-screen coords)         */
    int      curVisible = 1;
    bool     needRender = false;
    /* drag-to-scroll */
    int      dragStartY = 0, dragStartTop = 0;
    bool     dragging = false;
};

namespace {

/* Text of one live-screen row, exactly `cols` columns wide (blank/continuation
 * cells → space) so the grid stays aligned. */
std::string screenRowText(lcd_term_t* t, int sr) {
    std::string s;
    s.reserve((size_t)t->cols + 8);
    for (int c = 0; c < t->cols; ) {
        VTermScreenCell cell;
        VTermPos p; p.row = sr; p.col = c;
        if (vterm_screen_get_cell(t->scr, p, &cell)) {
            int w = cell.width < 1 ? 1 : cell.width;
            uint32_t ch = cell.chars[0];
            if (ch == 0 || ch == (uint32_t)-1) s.push_back(' ');
            else                               utf8Append(s, ch);
            c += w;
        } else { s.push_back(' '); c++; }
    }
    return s;
}

int maxTop(lcd_term_t* t) { return (int)t->sb.size(); }   /* bottom = live screen at window top 0 */

/* Fill the visible row labels from the window [top, top+rows) over the virtual
 * buffer (scrollback then live screen), and place the cursor. */
void renderView(lcd_term_t* t) {
    if (t->follow) t->top = maxTop(t);
    if (t->top < 0) t->top = 0;
    if (t->top > maxTop(t)) t->top = maxTop(t);
    const int nsb = (int)t->sb.size();
    for (int r = 0; r < t->rows; r++) {
        int v = t->top + r;
        if (v < nsb)                 lv_label_set_text(t->rowLbl[r], t->sb[v].c_str());
        else if (v < nsb + t->rows)  lv_label_set_text(t->rowLbl[r], screenRowText(t, v - nsb).c_str());
        else                         lv_label_set_text(t->rowLbl[r], "");
    }
    /* Cursor shows only on the live screen (when following). */
    bool curOnScreen = t->follow && t->curVisible && t->cur.row >= 0 && t->cur.row < t->rows;
    if (!curOnScreen) { lv_obj_add_flag(t->cursor, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(t->cursor, LV_OBJ_FLAG_HIDDEN);
    int winRow = (nsb - t->top) + t->cur.row;        /* live row 0 sits at window row (nsb-top) */
    lv_obj_set_pos(t->cursor, t->cur.col * t->charW, winRow * t->rowH);
}

/* ---- libvterm screen callbacks (fire during vterm_input_write) ---- */

int cbDamage(VTermRect, void* u)            { ((lcd_term_t*)u)->needRender = true; return 1; }
int cbMoveCursor(VTermPos pos, VTermPos, int visible, void* u) {
    auto* t = (lcd_term_t*)u; t->cur = pos; t->curVisible = visible; t->needRender = true; return 1;
}
int cbSetProp(VTermProp prop, VTermValue* val, void* u) {
    auto* t = (lcd_term_t*)u;
    if (prop == VTERM_PROP_CURSORVISIBLE) { t->curVisible = val->boolean; t->needRender = true; }
    return 1;
}
/* A line scrolled off the top of the live screen — keep it for scrollback. */
int cbSbPushline(int cols, const VTermScreenCell* cells, void* u) {
    auto* t = (lcd_term_t*)u;
    std::string s;
    s.reserve((size_t)cols + 4);
    for (int c = 0; c < cols; c++) {
        uint32_t ch = cells[c].chars[0];
        if (ch == 0 || ch == (uint32_t)-1) s.push_back(' ');
        else                               utf8Append(s, ch);
    }
    /* trim trailing blanks to save memory; keep at least empty */
    size_t end = s.find_last_not_of(' ');
    s.erase(end == std::string::npos ? 0 : end + 1);
    t->sb.push_back(std::move(s));
    if (t->sb.size() > SB_CAP) {
        t->sb.pop_front();
        if (!t->follow && t->top > 0) t->top--;   /* keep the scrolled-back view stable */
    }
    t->needRender = true;
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

    t->rowLbl.resize(t->rows);
    for (int r = 0; r < t->rows; r++) {
        lv_obj_t* l = lv_label_create(t->cont);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(l, 0, r * t->rowH);
        lv_obj_set_width(l, w);
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_style_text_color(l, fg, 0);
        lv_obj_set_style_text_line_space(l, 0, 0);
        lv_obj_set_style_text_letter_space(l, 0, 0);
        lv_label_set_text(l, "");
        t->rowLbl[r] = l;
    }

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
    vterm_input_write(t->vt, data, len);
    vterm_screen_flush_damage(t->scr);   /* deliver merged damage → our callbacks */
    if (t->needRender) {
        t->needRender = false;
        if (t->follow) renderView(t);    /* scrolled-back view stays put until you return */
    }
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
