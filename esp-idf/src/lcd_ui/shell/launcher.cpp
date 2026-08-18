/**
 * launcher.cpp — the phone shell's scrolling icon grid.
 *
 * Layout (stylesheet-driven): one vertically-scrolling flex-wrap grid of tiles
 * filling the screen below the status bar. Columns come from the panel width
 * (`minTilePx`); `launcher.rows` is the number of rows that must be reachable
 * without scrolling, so tiles divide the viewport evenly and anything past that
 * scrolls under a hairline scrollbar that rides inside the right-hand padding.
 * The grid scrolls by touch drag and by lcdScroll() edge-pan.
 *
 * Holding a tile still for 700 ms enters edit mode with that tile in hand: every
 * icon tilts back and forth through five stops between -20° and +20° at 10 fps
 * (slow on purpose — SPI), bar the one being held. In edit mode a tile is picked
 * up the moment it moves, dragging anywhere else scrolls the grid, and any tap
 * leaves edit mode. The order the tiles sit in is `s.lcd.launcher_order`.
 *
 * Tiles are added by shellLauncherAddTile(), called from lcdInstall()
 * (lcd_app.cpp) for every installed LcdApp.
 */
#include "shell_internal.h"
#include "stylesheet.h"
#include "lcd_internal.h"   /* lcdScreenW/H, lcdInputGroup, lcdIcon* */
#include "storage.h"
#include "log.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

lv_obj_t* s_root = nullptr;  /* launcher container (lower sibling of programs) */
lv_obj_t* s_grid = nullptr;  /* the scrolling tile grid */
lv_obj_t* s_drag = nullptr;  /* the tile currently held by a finger, or null */

struct Tile { LcdApp* app; lv_obj_t* tile; lv_obj_t* img; std::string basename; };
std::vector<Tile> s_tiles;   /* install order, never reordered — see applyOrder() */

int viewportW() { return lcdScreenW(); }
int viewportH() { return lcdScreenH() - lcdStyle().statusBar.h; }
/* Icon raster size for the current tile + zoom: the sheet base × UI scale. */
int iconPx() { return lcdPx(lcdStyle().launcher.iconPx); }

lv_point_t activePoint() {
    lv_point_t p = { 0, 0 };
    if (lv_indev_t* ind = lv_indev_active()) lv_indev_get_point(ind, &p);
    return p;
}

/* Derived grid: tiles size from the viewport, not the sheet. cols =
 * floor(usableW / minTile); tileW fills the row. tileH divides the viewport into
 * `launcher.rows` rows so that many always fit, and only falls back to the bare
 * icon+label height on a panel too short to hold them. Recomputed per tile add
 * so a wider panel or a bigger zoom simply reflows. */
struct Grid { int tileW, tileH; };
Grid gridFor() {
    const LcdStyle& st = lcdStyle();
    int padL = lcdPx(st.launcher.padLeft), padT = lcdPx(st.launcher.padTop);
    int padC = lcdPx(st.launcher.padCol),  padR = lcdPx(st.launcher.padRow);
    int minT = lcdPx(st.launcher.minTilePx);
    if (minT < 1) minT = 1;

    /* The scrollbar is a few px wide and sits inside the right-hand padding, so
     * the columns are laid out against the full padded width either way. */
    int availW = viewportW() - 2 * padL;
    int cols = availW / minT;
    if (cols < 1) cols = 1;
    int tileW = (availW - (cols - 1) * padC) / cols;

    int labelH = st.launcher.labelFont ? lv_font_get_line_height(st.launcher.labelFont) : lcdPx(14);
    int minH   = iconPx() + labelH;              /* icon + label, no breathing room */

    int rows   = st.launcher.rows > 0 ? st.launcher.rows : 1;
    int availH = viewportH() - 2 * padT;
    int tileH  = (availH - (rows - 1) * padR) / rows;
    if (tileH < minH) tileH = minH;

    return { tileW, tileH };
}

/* ---- tile order ----
 * `s.lcd.launcher_order` is a comma-separated list of LcdApp::Config::name, in
 * the order the tiles sit. A drag writes it; a hand-edit over the CLI or the
 * browser re-sorts the grid the same way.
 *
 * It is a *preference*, not the roster: an app the key doesn't name keeps its
 * install position, after the named ones, so a straddle added since the last
 * drag lands at the end instead of jumping the queue; a name for an app that
 * isn't installed is ignored. Names come from string literals in app code, so
 * they carry no commas of their own. */
constexpr const char* kOrderKey = "s.lcd.launcher_order";

/* Position of `name` among the key's comma-separated fields, or -1. Whole fields
 * only, so "Log" does not match inside "Logbook". */
int rankIn(const char* order, const char* name) {
    if (!name || !*name) return -1;
    size_t nlen = strlen(name);
    int idx = 0;
    for (const char* p = order; *p; idx++) {
        const char* end = strchr(p, ',');
        size_t flen = end ? (size_t)(end - p) : strlen(p);
        if (flen == nlen && strncmp(p, name, nlen) == 0) return idx;
        if (!end) break;
        p = end + 1;
    }
    return -1;
}

/* Sort the grid's children to match the key. Only ever moves children, so the
 * write a drop makes lands back here through the subscription and sorts to the
 * order already on screen — it settles rather than loops. */
void applyOrder() {
    if (!s_grid || s_tiles.empty()) return;
    char order[256];
    storageGetStr(kOrderKey, order, sizeof(order), "");

    std::vector<const Tile*> want;
    want.reserve(s_tiles.size());
    for (auto& t : s_tiles) if (t.tile) want.push_back(&t);
    /* Stable, and unnamed apps all rank last, so they keep install order. */
    std::stable_sort(want.begin(), want.end(), [&order](const Tile* a, const Tile* b) {
        int ra = rankIn(order, a->app->cfg().name);
        int rb = rankIn(order, b->app->cfg().name);
        return (ra < 0 ? INT_MAX : ra) < (rb < 0 ? INT_MAX : rb);
    });
    for (size_t i = 0; i < want.size(); i++)
        lv_obj_move_to_index(want[i]->tile, (int32_t)i);
}

void saveOrder() {
    if (!s_grid) return;
    char buf[256] = {};
    size_t n = 0;
    uint32_t cnt = lv_obj_get_child_count(s_grid);
    for (uint32_t i = 0; i < cnt; i++) {
        LcdApp* app = static_cast<LcdApp*>(lv_obj_get_user_data(lv_obj_get_child(s_grid, i)));
        const char* nm = app ? app->cfg().name : nullptr;
        if (!nm || !*nm) continue;
        int w = snprintf(buf + n, sizeof(buf) - n, "%s%s", n ? "," : "", nm);
        if (w < 0 || (size_t)w >= sizeof(buf) - n) { buf[n] = '\0'; break; }   /* keep what fits */
        n += (size_t)w;
    }
    storageSet(kOrderKey, buf);
}

/* ---- wiggle ----
 * One timer drives every tile: the whole grid is in the same phase, so the icons
 * tilt together. Five stops — -20°, -10°, 0°, +10°, +20° — walked out and back,
 * which is eight frames a cycle with each end visited once. 100 ms a frame
 * (10 fps) keeps a sweep at the 400 ms it takes with the ends alone, so the extra
 * stops smooth the motion instead of slowing it; that is twice the redraws for a
 * 320x240 SPI panel, and the ceiling of what it will take.
 * Rotation is in 0.1° units and pivots on the icon centre (set per tile). */
constexpr uint32_t kWiggleStepMs   = 100;
constexpr int32_t  kWiggleAngle[8] = { -200, -100, 0, 100, 200, 100, 0, -100 };

lv_timer_t* s_wiggle      = nullptr;
int         s_wigglePhase = 0;

void wiggleApply(int32_t angle) {
    for (auto& t : s_tiles) {
        if (!t.img) continue;
        /* The tile in hand holds still — the same cue iOS gives a lifted icon. */
        lv_image_set_rotation(t.img, t.tile == s_drag ? 0 : angle);
    }
}

void wiggleTick(lv_timer_t*) {
    s_wigglePhase = (s_wigglePhase + 1) & 7;
    wiggleApply(kWiggleAngle[s_wigglePhase]);
}

void wiggleToggle() {
    if (s_wiggle) {
        lv_timer_delete(s_wiggle);
        s_wiggle = nullptr;
        s_wigglePhase = 0;
        wiggleApply(0);
        return;
    }
    s_wigglePhase = 0;
    wiggleApply(kWiggleAngle[0]);   /* first frame now, not one step from now */
    s_wiggle = lv_timer_create(wiggleTick, kWiggleStepMs, nullptr);
}

/* ---- drag to reorder ----
 * A grabbed tile follows the finger by a style translate, which leaves its slot
 * reserved in the flex flow — that reserved slot IS the space that opens up, and
 * the rest of the grid reflows around it as the dragged tile's centre passes
 * theirs. On release the translate springs back to zero and the new order is
 * written to the key.
 *
 * A translate, not a free-floating object: flex owns the position, so a set_pos
 * would be overwritten on the next layout, and taking the tile out of the flow
 * (FLOATING) would close the gap rather than open one.
 *
 * The grid's own vertical scroll is off for the duration, so the gesture can't be
 * stolen mid-drag. The grab threshold is well inside LVGL's scroll limit, so the
 * flag is always off before a scroll could be decided. That leaves no way to
 * reach a slot below the fold, which is what the edge scroll below is for. */
constexpr int      kDragEdgePx   = 20;   /* hold the finger this near an edge to scroll */
constexpr int      kDragScrollPx = 6;    /* px per tick */
constexpr int      kDragSlopPx   = 3;    /* travel that grabs a tile in edit mode */
constexpr int      kTapSlopPx    = 10;   /* travel past which the gesture is not a tap */
constexpr uint32_t kEditHoldMs   = 700;  /* hold a tile this long to enter edit mode */

lv_timer_t* s_dragScroll    = nullptr;
int         s_grabDX = 0, s_grabDY = 0;    /* pointer − tile top-left when grabbed */
int         s_pressPX = 0, s_pressPY = 0;  /* where the finger went down */
/* The finger's last known position. lv_indev_active() is only live while LVGL is
 * processing an input event and reads back null from a plain timer, so the edge
 * scroll below cannot ask for the point itself — the event handlers leave it
 * here instead. */
int         s_lastPX = 0, s_lastPY = 0;
uint32_t    s_pressTick     = 0;           /* when it went down — our own hold clock */
bool        s_dragMoved     = false;
bool        s_suppressClick = false;       /* this gesture already meant something else */

/* Re-aim the translate at the finger. The base (layout) position is the current
 * coords minus the translate already applied — LVGL folds the translate into the
 * object's coords at layout time — so this stays right after a reindex or a
 * scroll has moved the slot out from under it.
 *
 * That subtraction is only valid while coords and the translate agree, and a
 * style write leaves them disagreeing until the next layout. So force one on the
 * way in and on the way out: a dozen flex children cost nothing to lay out, and
 * every caller is then free to read coords straight away. */
void dragFollow() {
    if (!s_drag) return;
    lv_obj_update_layout(s_grid);
    lv_area_t a;
    lv_obj_get_coords(s_drag, &a);
    int tx = lv_obj_get_style_translate_x(s_drag, LV_PART_MAIN);
    int ty = lv_obj_get_style_translate_y(s_drag, LV_PART_MAIN);
    lv_obj_set_style_translate_x(s_drag, (s_lastPX - s_grabDX) - (a.x1 - tx), 0);
    lv_obj_set_style_translate_y(s_drag, (s_lastPY - s_grabDY) - (a.y1 - ty), 0);
    lv_obj_update_layout(s_grid);
}

/* Where the dragged tile belongs now: the number of other tiles whose slot sorts
 * before its centre in reading order. Counting, rather than hit-testing the slot
 * under it, is what makes the LAST position reachable — the space after the final
 * icon belongs to no slot, so a hit-test finds nothing to trade places with, and
 * the same goes for the gaps between slots. */
int dragTargetIndex() {
    lv_area_t d;
    lv_obj_get_coords(s_drag, &d);
    int cx = (d.x1 + d.x2) / 2, cy = (d.y1 + d.y2) / 2;
    int band = (d.y2 - d.y1 + 1) / 2;   /* same row = centres within half a tile */
    int before = 0;
    uint32_t n = lv_obj_get_child_count(s_grid);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* c = lv_obj_get_child(s_grid, i);
        if (c == s_drag) continue;
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        int ox = (a.x1 + a.x2) / 2, oy = (a.y1 + a.y2) / 2;
        if (oy < cy - band || (oy <= cy + band && ox < cx)) before++;
    }
    return before;
}

void dragReindex() {
    if (!s_drag) return;
    int want = dragTargetIndex();
    if (want == (int)lv_obj_get_index(s_drag)) return;
    lv_obj_move_to_index(s_drag, (int32_t)want);
    dragFollow();   /* the slots moved — re-aim off the new one */
}

/* Held near the top or bottom edge: scroll the grid by hand (its own scrolling is
 * off mid-drag) so a tile can be carried to a row that isn't on screen. */
void dragScrollTick(lv_timer_t*) {
    if (!s_drag) return;
    lv_area_t g;
    lv_obj_get_coords(s_grid, &g);
    int dy = 0;
    if      (s_lastPY < g.y1 + lcdPx(kDragEdgePx)) dy = -lcdPx(kDragScrollPx);
    else if (s_lastPY > g.y2 - lcdPx(kDragEdgePx)) dy =  lcdPx(kDragScrollPx);
    if (!dy) return;
    lv_obj_scroll_by_bounded(s_grid, 0, -dy, LV_ANIM_OFF);
    dragFollow();
    dragReindex();
}

void dragBegin(lv_obj_t* tile) {
    if (s_drag || !tile) return;
    /* Dragging needs a device with a position. A keypad's ENTER can also long-
     * press a focused tile, and there is no point under it to follow. */
    lv_indev_t* ind = lv_indev_active();
    if (!ind || lv_indev_get_type(ind) != LV_INDEV_TYPE_POINTER) return;
    lv_anim_delete(tile, nullptr);   /* cancel a settle still in flight */
    lv_area_t a;
    lv_obj_get_coords(tile, &a);
    s_drag          = tile;
    s_suppressClick = true;        /* the release ends a drag, it doesn't tap */
    s_grabDX = s_lastPX - a.x1;
    s_grabDY = s_lastPY - a.y1;
    lv_obj_add_flag(tile, LV_OBJ_FLAG_PRESS_LOCK);      /* the press follows the finger off the tile */
    /* Off for the drag only, so the grid can't steal the gesture. The grab
     * threshold is below LVGL's scroll limit, so we always get there first. */
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    if (s_wiggle) wiggleApply(kWiggleAngle[s_wigglePhase]);   /* still the one in hand */
    s_dragScroll = lv_timer_create(dragScrollTick, 60, nullptr);
}

void editEnter() {
    if (!s_wiggle) wiggleToggle();
    s_suppressClick = true;
}

/* Slide the tile from wherever the finger left it into the slot it landed on. */
void dragSettle(lv_obj_t* tile) {
    int tx = lv_obj_get_style_translate_x(tile, LV_PART_MAIN);
    int ty = lv_obj_get_style_translate_y(tile, LV_PART_MAIN);
    if (!tx && !ty) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, tile);
    lv_anim_set_duration(&a, 140);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void* o, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t*)o, v, 0); });
    lv_anim_set_values(&a, tx, 0);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, [](void* o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t*)o, v, 0); });
    lv_anim_set_values(&a, ty, 0);
    lv_anim_start(&a);
}

void dragEnd() {
    if (!s_drag) return;
    lv_obj_t* tile = s_drag;
    s_drag = nullptr;
    if (s_dragScroll) { lv_timer_delete(s_dragScroll); s_dragScroll = nullptr; }
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    dragSettle(tile);
    if (s_dragMoved) saveOrder();
}

/* ---- tile events ----
 * The hold that enters edit mode is timed here rather than taken from LVGL's
 * LV_EVENT_LONG_PRESSED, because that threshold belongs to the whole indev and
 * would drag every other widget's long press along with it. LVGL sends PRESSING
 * on every read while a finger is down, moving or not, so the press timestamp is
 * all the clock this needs. */

void onTilePressed(lv_event_t*) {
    lv_point_t p = activePoint();
    s_pressPX = s_lastPX = p.x;
    s_pressPY = s_lastPY = p.y;
    s_pressTick     = lv_tick_get();
    s_dragMoved     = false;
    s_suppressClick = false;
}

void onTilePressing(lv_event_t* e) {
    lv_point_t p = activePoint();
    s_lastPX = p.x;
    s_lastPY = p.y;
    int dx = abs(p.x - s_pressPX), dy = abs(p.y - s_pressPY);
    /* Past the tap slop the gesture is a drag, whatever it ends up dragging — the
     * grid, a tile, or nothing at all when there is no overflow to scroll. None
     * of those is a tap, so none of them may open the app on release. A looser
     * threshold than the grab: a tap is allowed to wander a little. */
    if (dx > lcdPx(kTapSlopPx) || dy > lcdPx(kTapSlopPx)) s_suppressClick = true;

    bool moved = dx > lcdPx(kDragSlopPx) || dy > lcdPx(kDragSlopPx);
    if (!s_drag) {
        /* In edit mode a tile is picked up the instant it moves — no second hold.
         * Out of edit mode a plain drag stays the grid's to scroll, and only a
         * still hold enters. */
        if (s_wiggle) {
            if (!moved) return;
            dragBegin((lv_obj_t*)lv_event_get_target(e));
        } else {
            if (moved || lv_tick_elaps(s_pressTick) < kEditHoldMs) return;
            editEnter();
            dragBegin((lv_obj_t*)lv_event_get_target(e));
        }
        if (!s_drag) return;
    }
    if (moved) s_dragMoved = true;
    dragFollow();
    dragReindex();
}

void onTileReleased(lv_event_t*) { dragEnd(); }

void onTileClick(lv_event_t* e) {
    /* LVGL sends CLICKED on the release of a drag or an edit-mode entry too. */
    if (s_suppressClick) { s_suppressClick = false; return; }
    /* In edit mode a tap is how you leave it — icons don't open until you have. */
    if (s_wiggle) { wiggleToggle(); return; }
    LcdApp* app = static_cast<LcdApp*>(lv_event_get_user_data(e));
    if (app) { dbg("tile click '%s'\n", app->cfg().name); shellOpenApp(app); }
}

/* A tap on bare grid — the space past the last icon, or between two — leaves edit
 * mode as well, so it is never a hunt for the right thing to tap. */
void onGridClick(lv_event_t*) { if (s_wiggle) wiggleToggle(); }

}  // namespace

lv_obj_t* shellLauncherRoot(void) { return s_root; }

void shellLauncherInit(lv_obj_t* screen) {
    const LcdStyle& st = lcdStyle();
    s_root = lv_obj_create(screen);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_pos(s_root, 0, st.statusBar.h);
    lv_obj_set_size(s_root, viewportW(), viewportH());
    lv_obj_set_style_bg_color(s_root, lv_color_hex(st.launcher.bg), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_grid = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_pos(s_grid, 0, 0);
    lv_obj_set_size(s_grid, viewportW(), viewportH());
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(s_grid, lcdPx(st.launcher.padTop), 0);
    lv_obj_set_style_pad_bottom(s_grid, lcdPx(st.launcher.padTop), 0);
    lv_obj_set_style_pad_left(s_grid, lcdPx(st.launcher.padLeft), 0);
    lv_obj_set_style_pad_right(s_grid, lcdPx(st.launcher.padLeft), 0);
    lv_obj_set_style_pad_row(s_grid, lcdPx(st.launcher.padRow), 0);
    lv_obj_set_style_pad_column(s_grid, lcdPx(st.launcher.padCol), 0);
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_CLICKABLE);   /* only to hear the tap below */
    lv_obj_add_event_cb(s_grid, onGridClick, LV_EVENT_CLICKED, nullptr);

    /* Hairline scrollbar: it has to live inside the right-hand padding without
     * stealing a column, so it is a few px wide rather than the theme's slab. */
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(s_grid, lcdPx(3), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_grid, lcdPx(2), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_grid, lv_color_hex(0x5A6470), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(s_grid, lcdPx(1), LV_PART_SCROLLBAR);

    NOW_AND_ON_CHANGE(kOrderKey, { applyOrder(); });
}

void shellLauncherAddTile(LcdApp* app) {
    if (!s_grid || !app) return;
    const LcdStyle& st = lcdStyle();
    Grid g = gridFor();

    /* Tile: a flex-column button (icon over label), so internals flow with
     * padding instead of magic TOP_MID/BOTTOM_MID offsets. */
    lv_obj_t* tile = lv_button_create(s_grid);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, g.tileW, g.tileH);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tile, lcdPx(4), 0);
    lv_obj_set_style_pad_top(tile, lcdPx(4), 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(tile, app);   /* saveOrder() reads the order off the children */
    lv_obj_add_event_cb(tile, onTilePressed,   LV_EVENT_PRESSED,      nullptr);
    lv_obj_add_event_cb(tile, onTilePressing,  LV_EVENT_PRESSING,     nullptr);
    lv_obj_add_event_cb(tile, onTileReleased,  LV_EVENT_RELEASED,     nullptr);
    lv_obj_add_event_cb(tile, onTileReleased,  LV_EVENT_PRESS_LOST,   nullptr);
    lv_obj_add_event_cb(tile, onTileClick,     LV_EVENT_CLICKED,      app);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), tile);

    lv_obj_t* img = lv_image_create(tile);   /* src set when the icon loads */
    lv_obj_set_size(img, iconPx(), iconPx());   /* stable slot before the raster lands */
    lv_image_set_pivot(img, iconPx() / 2, iconPx() / 2);   /* wiggle turns on centre */

    lv_obj_t* lbl = lv_label_create(tile);
    lv_label_set_text(lbl, app->cfg().name ? app->cfg().name : "");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    if (st.launcher.labelFont) lv_obj_set_style_text_font(lbl, st.launcher.labelFont, 0);
    lv_obj_set_width(lbl, g.tileW);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    const char* base = app->cfg().iconBasename ? app->cfg().iconBasename : "";
    info("add tile '%s' (icon '%s')\n", app->cfg().name ? app->cfg().name : "", base);
    s_tiles.push_back({ app, tile, img, base });
    applyOrder();   /* the new tile lands where the key wants it, not just last */

    lcdIconRequest(base, iconPx());   /* -> lcdLauncherIconLoaded */
}

void lcdLauncherIconLoaded(const char* basename, int px) {
    if (!basename || px != iconPx()) return;   /* stale (pre-zoom) size: ignore */
    const lv_image_dsc_t* dsc = lcdIconDsc(basename, px);
    if (!dsc) return;
    for (auto& t : s_tiles)
        if (t.img && t.basename == basename) lv_image_set_src(t.img, dsc);
    lcdBootSettleKick();   /* an icon landed — push the boot backlight reveal out */
}
