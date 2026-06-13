/**
 * lcd_textview.cpp — virtualized monospace text view (lcdTextView* in lcd.h).
 *
 * Holds an arbitrarily large scrollback in a std::string but only ever lays out
 * the on-screen window into a single LVGL label, so both append and scroll cost
 * O(visible rows) rather than O(scrollback). The standard "don't put a megabyte
 * of text in one widget" virtual scroll: an invisible spacer child gives the
 * scroll container its true content height (so LVGL's own scrollbar + scroll
 * physics work unchanged), while the label is repositioned and refilled with
 * just the rows around the current scroll offset — plus a small margin — as the
 * view moves.
 *
 * Monospace is assumed for the column math: text is hard-wrapped at a fixed
 * column count (terminal-style, not word-wrapped), and the row pitch is the font
 * line height with zero line spacing — so the spacer's pixel height (rows x
 * pitch) stays exact and scrolling never drifts against LVGL's own layout.
 *
 * Rendering is debounced, not synchronous. A panel flush is the bottleneck here
 * (the SPI transfer paints the whole text region at only a few Hz), so the win
 * isn't rate-limiting draws — it's *skipping* the intermediate ones. Append /
 * set / scroll only mark the view dirty and (re)arm a paused lv_timer that fires
 * once activity has settled for SETTLE_MS; the timer then does the single reflow
 * + repaint of the *latest* state and re-pauses. A boot-time log flood therefore
 * paints once when it quiets instead of churning through every scroll position,
 * and a drag redraws when the finger stops, not on every move. A MAX_DEFER_MS
 * cap bounds the wait so a non-stop stream still shows periodic progress. The
 * timer pausing when clean keeps the lcd task's event-driven / light-sleep loop
 * intact (no idle render wake).
 *
 * Runs on the lcd task (LVGL is single-threaded); see lcd.h / lcd_internal.h.
 */
#include "lcd.h"
#include "lvgl.h"

#include <string>
#include <vector>
#include <cstdint>

struct lcd_textview_t {
    lv_obj_t*             cont   = nullptr;  /* scroll container (the viewport)     */
    lv_obj_t*             spacer = nullptr;  /* invisible child = true content height*/
    /* Double-buffered band containers: [active] shown, other is back. Each holds
     * pooled labels — one per run of consecutive same-colour rows; with no line
     * colorizer that's exactly one label per band (the old single-label design). */
    lv_obj_t*             band[2] = { nullptr, nullptr };
    std::vector<lv_obj_t*> pool[2];          /* per-band label pools (lazy-created) */
    int                   active = 0;        /* index of the visible (front) band   */
    lcd_textview_line_color_cb lineColor = nullptr;  /* optional per-line colour    */
    lv_color_t            fg{};              /* default text colour                 */
    int                   width  = 0;        /* label width (viewport minus pad)    */
    const lv_font_t*      font   = nullptr;
    int                   rowH   = 8;        /* line pitch (px)                     */
    int                   cols   = 1;        /* chars per visual row                */
    int                   vpRows = 1;        /* rows that fit in the viewport       */
    size_t                budget = 4096;     /* scrollback cap (bytes)              */
    std::string           buf;               /* committed scrollback (capped)       */
    std::string           suffix;            /* transient, un-stored tail           */
    std::string           disp;              /* buf + suffix (the slice source)     */
    std::vector<uint32_t> rowStart;          /* byte offset of each visual row      */
    int                   firstRow  = -1;    /* first row currently in the label    */
    int                   rowsShown = 0;

    /* Debounced rendering. Mutators only set the dirty flags below and (re)arm the
     * flush timer; it fires once activity settles, does the one reflow/render of
     * the latest state, and re-pauses. Skips the intermediate states of a flood. */
    lv_timer_t* flushTimer  = nullptr;
    uint32_t    dirtySince  = 0;             /* lv_tick when batch went dirty; 0=clean */
    bool        needReflow  = false;         /* buf/suffix changed → re-wrap        */
    bool        needRender  = false;         /* scroll moved → re-materialise band  */
    bool        stick       = false;         /* re-pin to bottom on flush           */
    bool        haveAnchor  = false;         /* re-pin to anchorOff on flush        */
    bool        captured    = false;         /* pre-batch scroll state captured     */
    size_t      anchorOff   = 0;             /* top visible row offset (pre-batch)  */
    size_t      erased      = 0;             /* bytes front-trimmed this batch      */
};

namespace {

/* Rows rendered above + below the viewport, so small scrolls don't re-lay-out
 * (and so don't cost a panel flush) at all. 32 each side = a ~64-row band, well
 * past a viewport, so a normal swipe stays inside it: pure LVGL scroll of the
 * already-materialised label, no re-materialise, no roll-through. */
constexpr int      MARGIN       = 32;
/* Debounce: draw only after activity has been quiet this long... */
constexpr uint32_t SETTLE_MS    = 100;
/* ...but never defer a pending draw longer than this, so a non-stop stream
 * still updates (~1 Hz) instead of looking frozen. */
constexpr uint32_t MAX_DEFER_MS = 1000;

/* Rebuild disp = buf + suffix, recompute the visual-row offset table (hard wrap
 * at `cols`), and resize the spacer to the true content height. */
void reflow(lcd_textview_t* v) {
    v->disp.clear();
    v->disp.reserve(v->buf.size() + v->suffix.size());
    v->disp.append(v->buf);
    v->disp.append(v->suffix);

    v->rowStart.clear();
    const std::string& s = v->disp;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        v->rowStart.push_back((uint32_t)i);
        int col = 0;
        while (i < n && s[i] != '\n' && col < v->cols) { i++; col++; }
        if (i < n && s[i] == '\n') i++;   /* the newline belongs to this row */
    }
    const int total = (int)v->rowStart.size();
    lv_obj_set_height(v->spacer, total > 0 ? total * v->rowH : 1);
}

/* Materialise the rows around the current scroll offset into the back band.
 * Skips the work unless the visible band moved outside the rendered window (or
 * force). */
/* Reveal the freshly-rendered back band and hide the stale front one in one
 * step, so a band re-materialise appears atomically instead of repainting in
 * place. The viewport content is identical across the swap (same rows under the
 * scroll offset, just backed by a differently-positioned band), so it costs at
 * most one flush of the viewport region — and only on a band cross. We swap via
 * the HIDDEN flag rather than z-order: the bands are transparent (the container
 * paints the background), so a back band left visible would composite through. */
void swapBands(lcd_textview_t* v) {
    lv_obj_remove_flag(v->band[1 - v->active], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag   (v->band[v->active],     LV_OBJ_FLAG_HIDDEN);
    v->active = 1 - v->active;
}

/* Colour of the logical line containing byte offset `lineStart` (which must be
 * the line's first byte). LCD_TEXTVIEW_DEFAULT when no colorizer is set. */
uint32_t lineColorAt(lcd_textview_t* v, size_t lineStart) {
    if (!v->lineColor) return LCD_TEXTVIEW_DEFAULT;
    size_t end = v->disp.find('\n', lineStart);
    if (end == std::string::npos) end = v->disp.size();
    return v->lineColor(v->disp.data() + lineStart, end - lineStart);
}

/* Emit one group of consecutive same-colour rows as a (pooled) label in the
 * back band. `bi` is the back-band index, `li` the next free pool slot. */
void emitGroup(lcd_textview_t* v, int bi, size_t& li, int yRow,
               const std::string& text, uint32_t color) {
    auto& pool = v->pool[bi];
    if (li >= pool.size()) {
        lv_obj_t* l = lv_label_create(v->band[bi]);
        /* Rows are pre-wrapped to <= cols with explicit \n, so WRAP mode only
         * wastes time re-measuring word breaks over the whole label on every
         * draw (×32 partial strips). CLIP splits on \n and skips that. */
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_font(l, v->font, 0);
        lv_obj_set_style_text_line_space(l, 0, 0);
        lv_obj_set_style_text_letter_space(l, 0, 0);
        pool.push_back(l);
    }
    lv_obj_t* l = pool[li++];
    lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(l, 0, yRow * v->rowH);
    lv_obj_set_width(l, v->width);
    lv_obj_set_style_text_color(
        l, color == LCD_TEXTVIEW_DEFAULT ? v->fg : lv_color_hex(color), 0);
    lv_label_set_text(l, text.c_str());
}

void renderWindow(lcd_textview_t* v, bool force) {
    const int bi = 1 - v->active;               /* materialise into the hidden band */
    lv_obj_t* back = v->band[bi];
    if (!back || !lv_obj_is_valid(back)) return;
    const int total = (int)v->rowStart.size();
    if (total == 0) {
        for (lv_obj_t* l : v->pool[bi]) lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(back, 0);
        v->firstRow  = 0;
        v->rowsShown = 0;
        swapBands(v);
        return;
    }

    int scrollY = lv_obj_get_scroll_y(v->cont);
    if (scrollY < 0) scrollY = 0;
    const int topRow = scrollY / v->rowH;
    const int botRow = topRow + v->vpRows;

    if (!force && v->firstRow >= 0 &&
        topRow >= v->firstRow && botRow <= v->firstRow + v->rowsShown)
        return;                            /* still inside the rendered band — no swap */

    /* Slide a fixed-size window to fit within the content, so an elastic
     * overscroll past either end never shrinks or blanks the visible rows. */
    int shown = v->vpRows + 2 * MARGIN;
    if (shown > total) shown = total;
    int first = topRow - MARGIN;
    if (first > total - shown) first = total - shown;
    if (first < 0) first = 0;

    /* The window's head row may be a continuation of a wrapped logical line —
     * its colour comes from the line head, which sits before the window. */
    uint32_t color = LCD_TEXTVIEW_DEFAULT;
    if (v->lineColor) {
        size_t s0 = v->rowStart[first];
        size_t lineStart = s0;
        if (s0 > 0 && v->disp[s0 - 1] != '\n') {
            size_t nl = v->disp.rfind('\n', s0 - 1);
            lineStart = (nl == std::string::npos) ? 0 : nl + 1;
        }
        color = lineColorAt(v, lineStart);
    }

    /* Group consecutive same-colour rows into one label each; the whole band
     * is a single group (one label) when no colorizer is installed. */
    size_t li = 0;
    std::string group;
    int groupRow = first, groupRows = 0;
    for (int r = first; r < first + shown; r++) {
        const uint32_t s0 = v->rowStart[r];
        if (v->lineColor && r > first && (s0 == 0 || v->disp[s0 - 1] == '\n')) {
            uint32_t c = lineColorAt(v, s0);
            if (c != color) {
                emitGroup(v, bi, li, groupRow - first, group, color);
                group.clear();
                groupRow = r; groupRows = 0;
                color = c;
            }
        }
        uint32_t s1 = (r + 1 < total) ? v->rowStart[r + 1] : (uint32_t)v->disp.size();
        if (s1 > s0 && v->disp[s1 - 1] == '\n') s1--;   /* drop the row's newline */
        if (groupRows++) group.push_back('\n');         /* blank rows still separate */
        group.append(v->disp, s0, s1 - s0);
    }
    emitGroup(v, bi, li, groupRow - first, group, color);
    for (; li < v->pool[bi].size(); li++)
        lv_obj_add_flag(v->pool[bi][li], LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_y(back, first * v->rowH);
    lv_obj_set_height(back, shown * v->rowH);
    v->firstRow  = first;
    v->rowsShown = shown;
    swapBands(v);
}

/* Trim buf to budget on a whole-line boundary; returns bytes removed from front. */
size_t trim(lcd_textview_t* v) {
    if (v->buf.size() <= v->budget) return 0;
    size_t cut = v->buf.size() - v->budget;
    const size_t nl = v->buf.find('\n', cut);
    cut = (nl == std::string::npos) ? v->buf.size() : nl + 1;
    v->buf.erase(0, cut);
    return cut;
}

/* (Re)arm the flush timer to fire SETTLE_MS after this — the latest — event, so
 * a burst coalesces into one draw once it quiets. The MAX_DEFER_MS cap shortens
 * the wait as the batch ages, so a stream that never quiets still draws ~1 Hz. */
void schedule(lcd_textview_t* v) {
    if (!v->flushTimer) return;
    if (v->dirtySince == 0) { uint32_t t = lv_tick_get(); v->dirtySince = t ? t : 1; }
    const uint32_t age = lv_tick_elaps(v->dirtySince);
    uint32_t period = SETTLE_MS;
    if (age + SETTLE_MS >= MAX_DEFER_MS)
        period = (age >= MAX_DEFER_MS) ? 1 : (MAX_DEFER_MS - age);
    lv_timer_set_period(v->flushTimer, period);
    lv_timer_reset(v->flushTimer);     /* restart the countdown from now */
    lv_timer_resume(v->flushTimer);
}

/* Capture the pre-batch scroll intent once, before the first mutation of a batch:
 * pin to bottom if we were at it, else remember the top row so a front-trim can
 * re-pin to the same content. Idempotent within a batch (cleared on flush). */
void captureAnchor(lcd_textview_t* v) {
    if (v->captured) return;
    v->captured = true;
    if (lcdTextViewAtBottom(v)) { v->stick = true; return; }
    if (v->rowStart.empty()) return;
    int scrollY = lv_obj_get_scroll_y(v->cont);
    if (scrollY < 0) scrollY = 0;
    int topRow = scrollY / v->rowH;
    if (topRow >= (int)v->rowStart.size()) topRow = (int)v->rowStart.size() - 1;
    v->anchorOff  = v->rowStart[topRow];
    v->haveAnchor = true;
}

/* Apply all pending work in one pass: re-wrap, re-pin the scroll, repaint. */
void flush(lcd_textview_t* v) {
    if (v->needReflow) reflow(v);

    if (v->stick) {
        lv_obj_scroll_to_y(v->cont, LV_COORD_MAX, LV_ANIM_OFF);
    } else if (v->needReflow && v->haveAnchor) {
        const size_t target = (v->anchorOff > v->erased) ? (v->anchorOff - v->erased) : 0;
        int lo = 0, hi = (int)v->rowStart.size();
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (v->rowStart[mid] < target) lo = mid + 1; else hi = mid;
        }
        int row = lo;
        if (row >= (int)v->rowStart.size()) row = (int)v->rowStart.size() - 1;
        if (row < 0) row = 0;
        lv_obj_scroll_to_y(v->cont, row * v->rowH, LV_ANIM_OFF);
    }

    if (v->needReflow || v->needRender) renderWindow(v, v->needReflow);

    v->needReflow = v->needRender = false;
    v->stick = v->haveAnchor = v->captured = false;
    v->erased = 0;
    v->dirtySince = 0;
}

void flushTimerCb(lv_timer_t* t) {
    flush(static_cast<lcd_textview_t*>(lv_timer_get_user_data(t)));
    lv_timer_pause(t);                 /* nothing pending until the next mutation */
}

void onScroll(lv_event_t* e) {
    auto* v = static_cast<lcd_textview_t*>(lv_event_get_user_data(e));
    v->needRender = true;
    schedule(v);
}
void onDelete(lv_event_t* e) {
    auto* v = static_cast<lcd_textview_t*>(lv_event_get_user_data(e));
    if (v->flushTimer) lv_timer_delete(v->flushTimer);
    delete v;
}

}  // namespace

lcd_textview_t* lcdTextViewCreate(lv_obj_t* parent, int32_t w, int32_t h,
                                  const lv_font_t* font, lv_color_t fg, size_t budget) {
    lcd_textview_t* v = new lcd_textview_t();
    v->font   = font;
    v->budget = budget ? budget : 4096;

    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, w, h);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cont, 1, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
    /* Terminal scrolling, not widget scrolling: kill the inertial throw and the
     * elastic overscroll. Momentum keeps animating scroll_y for seconds after a
     * swipe, and on a few-Hz panel each decay step is its own near-identical
     * draw — so a single swipe paints screens for ages. We want the view to stop
     * dead where the finger lifts. */
    lv_obj_remove_flag(cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLL_MOMENTUM |
                                             LV_OBJ_FLAG_SCROLL_ELASTIC));
    v->cont = cont;

    /* Invisible child whose height is the real content height — gives `cont` its
     * scroll extent (and a correctly-sized scrollbar) without holding any text. */
    lv_obj_t* sp = lv_obj_create(cont);
    lv_obj_remove_style_all(sp);
    lv_obj_set_pos(sp, 0, 0);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    v->spacer = sp;

    /* Two stacked band containers, double-buffered: renderWindow materialises
     * the next band into the hidden one and swaps visibility, so a band cross
     * appears atomically instead of repainting in place. Each holds only the
     * visible window ± MARGIN rows, as pooled child labels — one per run of
     * same-colour rows (exactly one label when no line colorizer is set). */
    v->fg    = fg;
    v->width = w - 2;
    for (int i = 0; i < 2; i++) {
        lv_obj_t* band = lv_obj_create(cont);
        lv_obj_remove_style_all(band);
        lv_obj_set_pos(band, 0, 0);
        lv_obj_set_size(band, w - 2, 1);
        lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE);
        if (i == 1) lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);  /* back buffer starts hidden */
        v->band[i] = band;
    }
    v->active = 0;

    v->rowH = lv_font_get_line_height(font);
    if (v->rowH < 1) v->rowH = 8;
    int charW = lv_font_get_glyph_width(font, 'M', 0);
    if (charW < 1) charW = 5;
    v->cols = (w - 2) / charW;
    if (v->cols < 1) v->cols = 1;
    v->vpRows = (h - 2) / v->rowH;
    if (v->vpRows < 1) v->vpRows = 1;

    lv_obj_add_event_cb(cont, onScroll, LV_EVENT_SCROLL, v);
    lv_obj_add_event_cb(cont, onDelete, LV_EVENT_DELETE, v);

    /* Paused until a mutation/scroll marks the view dirty; schedule() then sets
     * the period (the debounce/cap delay) and arms it. */
    v->flushTimer = lv_timer_create(flushTimerCb, SETTLE_MS, v);
    lv_timer_pause(v->flushTimer);
    return v;
}

void lcdTextViewAppend(lcd_textview_t* v, const char* data, size_t len) {
    if (!v || !data || len == 0) return;
    captureAnchor(v);                  /* pins bottom, or remembers the top row */
    v->buf.append(data, len);
    v->erased += trim(v);
    v->needReflow = true;
    schedule(v);
}

void lcdTextViewSet(lcd_textview_t* v, const char* data, size_t len) {
    if (!v) return;
    captureAnchor(v);                  /* only the bottom-stick intent survives  */
    v->buf.assign(data ? data : "", data ? len : 0);
    trim(v);
    v->haveAnchor = false;             /* content replaced — old offsets are void */
    v->erased = 0;
    v->needReflow = true;
    schedule(v);
}

void lcdTextViewSetSuffix(lcd_textview_t* v, const char* data, size_t len) {
    if (!v) return;
    captureAnchor(v);
    v->suffix.assign(data ? data : "", data ? len : 0);
    v->needReflow = true;
    schedule(v);
}

void lcdTextViewScrollToBottom(lcd_textview_t* v) {
    if (!v) return;
    v->stick = v->captured = true;     /* force bottom over any pending anchor */
    v->needRender = true;
    schedule(v);
}

bool lcdTextViewAtBottom(lcd_textview_t* v) {
    if (!v) return true;
    return lv_obj_get_scroll_bottom(v->cont) <= v->rowH;
}

lv_obj_t* lcdTextViewObj(lcd_textview_t* v) { return v ? v->cont : nullptr; }

void lcdTextViewSetLineColor(lcd_textview_t* v, lcd_textview_line_color_cb cb) {
    if (!v) return;
    v->lineColor = cb;
    v->firstRow  = -1;          /* invalidate the band so the repaint isn't skipped */
    v->needRender = true;
    schedule(v);
}

void lcdTextViewDelete(lcd_textview_t* v) {
    /* The container's LV_EVENT_DELETE handler (onDelete) frees `v` itself. */
    if (v && v->cont && lv_obj_is_valid(v->cont)) lv_obj_delete(v->cont);
}
