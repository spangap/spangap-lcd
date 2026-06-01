/**
 * lcd_launcher.cpp — phone-style launcher + program-layer lifecycle.
 *
 * Layer model: the launcher (icon grid) and each program's layer are children
 * of the active screen; the status bar and the bottom home-bar live on
 * lv_layer_top(), which always draws above screen children — so program layers
 * naturally sit between the launcher and the status bar with no manual
 * z-ordering. Tapping a tile creates the program's layer (calling its
 * registered fn once) or re-shows a kept one; a swipe up from the home-bar (or
 * lcdGoHome) hides it back to the launcher.
 */
#include "lcd_internal.h"

#include "log.h"

#include <vector>
#include <string>
#include <utility>
#include <cstdint>

namespace {

struct Entry {
    std::string name;
    std::string basename;
    lcd_fn_t    fn  = nullptr;
    lv_obj_t*   img = nullptr;   /* icon image inside the tile */
};

std::vector<Entry> s_entries;
lv_obj_t* s_screen   = nullptr;
lv_obj_t* s_launcher = nullptr;   /* icon grid (bottom) */
lv_obj_t* s_homebar  = nullptr;   /* bottom swipe-up strip (on top layer) */
lv_obj_t* s_bottomLine = nullptr; /* 1px near-white edge that rides the window's bottom */
lv_obj_t* s_current  = nullptr;   /* currently shown program layer */
lv_obj_t* s_fullscreenLayer = nullptr;  /* layer that asked to hide the status bar */

/* Home-bar drag state (manual, see the handlers below). */
int  s_dragStartY = 0;            /* finger y when the drag began */
int  s_dragBaseY  = 0;            /* layer y when the drag began */
bool s_dragging   = false;

/* 4 columns × 3 rows on a 320×240 panel (status bar steals 24px → 320×216 grid).
 * Cols: ROW_WRAP wraps when 5·TILE_W exceeds the width, so 64 < TILE_W ≤ 80 gives
 * exactly 4 (72 → 288px of tiles, the rest is SPACE_EVENLY gaps). Rows: pad_top 8
 * + 3·64 + 2·pad_row 8 = 216, an exact fit. Icons render at their native size (the
 * lcd_icons bucket is tile-sized — LAUNCHER_ICON_RES) with no runtime scaling: at
 * 36px the icon (y 4..40) sits clear of the Montserrat-12 label (y ~47..62). */
#define TILE_W     72
#define TILE_H     64

/* Program layers carry their entry index + 1 in user_data, so the LVGL tree
 * itself is the source of truth for "is this app's widget still alive". Reuse
 * is a scan (findProgramLayer), never a stored pointer — so evicting an app is
 * just lv_obj_del(layer): the next open finds nothing and rebuilds, with no
 * dangling pointer to clear. (+1 because 0 == untagged children, e.g. the
 * launcher grid, which is also a child of s_screen.) */
lv_obj_t* findProgramLayer(size_t idx) {
    uint32_t n = lv_obj_get_child_count(s_screen);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* c = lv_obj_get_child(s_screen, i);
        if ((size_t)(intptr_t)lv_obj_get_user_data(c) == idx + 1) return c;
    }
    return nullptr;
}

/* If the shown layer is deleted (e.g. evicted), don't leave s_current dangling;
 * reuse correctness is already handled by the scan above. */
void onLayerDelete(lv_event_t* e) {
    lv_obj_t* t = (lv_obj_t*)lv_event_get_target(e);
    if (t == s_current)         s_current = nullptr;
    if (t == s_fullscreenLayer) s_fullscreenLayer = nullptr;
}

/* Status bar + current-layer geometry both follow whichever layer (if any) asked
 * to go fullscreen via lcdProgramFullscreen(). Re-evaluated whenever the shown
 * layer changes (open / Home), so a fullscreen program reclaims the bar's 24px
 * only while it is the one on screen, and the bar returns when it leaves. */
void applyFullscreen() {
    bool active = s_fullscreenLayer && s_current == s_fullscreenLayer;
    lcdStatusbarSetVisible(!active);
    if (s_current) {
        int top = active ? 0 : LCD_STATUSBAR_H;
        lv_obj_set_pos(s_current, 0, top);
        lv_obj_set_size(s_current, lcdScreenW(), lcdScreenH() - top);
    }
    /* Trackball→arrows follows the shown layer the same way (the program marks
     * its layer with LV_OBJ_FLAG_USER_1 via lcdProgramScrollwheelArrows). */
    lcdScrollwheelArrowsApply(s_current && lv_obj_has_flag(s_current, LV_OBJ_FLAG_USER_1));
}

lv_obj_t* makeProgramLayer() {
    lv_obj_t* L = lv_obj_create(s_screen);
    lv_obj_remove_style_all(L);
    lv_obj_set_pos(L, 0, LCD_STATUSBAR_H);
    lv_obj_set_size(L, lcdScreenW(), lcdScreenH() - LCD_STATUSBAR_H);
    lv_obj_set_style_bg_color(L, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(L, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(L, onLayerDelete, LV_EVENT_DELETE, nullptr);
    return L;
}

/* The home-bar + bottom hairline ride the bottom edge of the current program
 * layer so they slide up with it as it's dragged toward Home. The pill shows
 * whenever an app is up (showHomebar); the hairline only while dragging (showLine). */
/* dy = how far the window is dragged up (<=0); 0 = at rest. The window's bottom is
 * always the screen bottom at rest, so place the bar/edge purely off screenH + dy —
 * never read the layer's coords, which are stale until the next layout (that's what
 * parked the bar ~24px too high right after open). */
void placeChrome(int dy = 0) {
    int barH = lcdScreenH() / 10;                  /* matches the home-bar height in init */
    if (s_homebar)    lv_obj_set_y(s_homebar,    lcdScreenH() - barH + dy);
    if (s_bottomLine) lv_obj_set_y(s_bottomLine, lcdScreenH() - 1   + dy);
}
void setHidden(lv_obj_t* o, bool hidden) {
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
void showHomebar(bool on) { setHidden(s_homebar, !on); }    /* pill: visible whenever an app is up */
void showLine(bool on)    { setHidden(s_bottomLine, !on); } /* edge: only while dragging the window up */

void openEntry(size_t idx) {
    if (idx >= s_entries.size()) return;
    lv_obj_t* layer = findProgramLayer(idx);
    bool firstBuild = (layer == nullptr);
    if (firstBuild) {                                /* first open, or rebuilt after evict */
        layer = makeProgramLayer();
        lv_obj_set_user_data(layer, (void*)(intptr_t)(idx + 1));
    }
    lv_anim_delete(layer, nullptr);                  /* cancel an in-flight Home slide */
    lv_obj_t* prev = s_current;
    /* Make it current BEFORE building so a program's fn can set its own layer
     * properties (lcdProgramFullscreen / lcdProgramScrollwheelArrows) and have
     * them bind to this layer. */
    s_current = layer;
    if (firstBuild && s_entries[idx].fn) s_entries[idx].fn(layer);   /* build into it, once */
    if (prev && prev != layer)
        lv_obj_add_flag(prev, LV_OBJ_FLAG_HIDDEN);
    applyFullscreen();                               /* pos/size + status bar for this layer */
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_HIDDEN);
    showHomebar(true);                               /* pill belongs to a shown app (edge stays hidden) */
    placeChrome();
}

void onTileClick(lv_event_t* e) {
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx < s_entries.size()) dbg("tile click '%s'\n", s_entries[idx].name.c_str());
    openEntry(idx);
}

/* Manual home-bar drag. LVGL's own gesture detection proved unreliable on a
 * strip this short, so we drive it by hand: PRESS_LOCK keeps the whole press on
 * the bar, PRESSING drags the current app layer up under the finger, and the
 * release decides — slide to Home if it ended in the top quarter, else spring the
 * layer back. No app shown = nothing to drag. */
int activePointerY() {
    lv_indev_t* ind = lv_indev_active();
    if (!ind) return 0;
    lv_point_t p;
    lv_indev_get_point(ind, &p);
    return p.y;
}

void homebarPressed(lv_event_t*) {
    s_dragging = false;
    if (!s_current) return;                  /* at the launcher: nothing to pull down */
    lv_anim_delete(s_current, nullptr);      /* cancel any in-flight spring / Home slide */
    s_dragStartY = activePointerY();
    s_dragBaseY  = lv_obj_get_y(s_current);
    s_dragging   = true;
    showLine(true);                          /* the white edge appears as the drag begins */
    placeChrome();
}

void homebarPressing(lv_event_t*) {
    if (!s_dragging || !s_current) return;
    int dy = activePointerY() - s_dragStartY;
    if (dy > 0) dy = 0;                       /* up only — never drag below the resting position */
    lv_obj_set_y(s_current, s_dragBaseY + dy);
    placeChrome(dy);                          /* home-bar + edge follow the window up */
}

void homebarReleased(lv_event_t* e) {
    if (!s_dragging || !s_current) { s_dragging = false; showLine(false); return; }
    s_dragging = false;
    /* Commit only on a real release whose finger ended in the top quarter; a lost
     * press (e.g. another indev) always springs back. */
    if (lv_event_get_code(e) == LV_EVENT_RELEASED &&
        activePointerY() < lcdScreenH() / 4) {
        lcdGoHomeInternal();                  /* slides from the dragged y to the top, hides, reveals launcher
                                                 (the edge rides along and is dropped in homeSlideDone) */
        return;
    }
    showLine(false);                          /* gesture ended without going Home — drop the edge */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_current);
    lv_anim_set_exec_cb(&a, [](void* v, int32_t y) { lv_obj_set_y((lv_obj_t*)v, y); placeChrome((int)(y - s_dragBaseY)); });
    lv_anim_set_values(&a, lv_obj_get_y(s_current), s_dragBaseY);
    lv_anim_set_duration(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Home slide-up: once the animation finishes, hide the layer and re-park it at
 * its normal position so the next open shows it in place. */
void homeSlideDone(lv_anim_t* a) {
    lv_obj_t* layer = (lv_obj_t*)a->var;
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(layer, LCD_STATUSBAR_H);
    showHomebar(false); showLine(false);     /* back at the launcher: no window, no bar/edge */
}

}  // namespace

void lcdLauncherInit(lv_obj_t* screen) {
    s_screen = screen;

    s_launcher = lv_obj_create(screen);
    lv_obj_remove_style_all(s_launcher);
    lv_obj_set_pos(s_launcher, 0, LCD_STATUSBAR_H);
    lv_obj_set_size(s_launcher, lcdScreenW(), lcdScreenH() - LCD_STATUSBAR_H);
    lv_obj_set_style_bg_color(s_launcher, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(s_launcher, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_launcher, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_launcher, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(s_launcher, 8, 0);
    lv_obj_set_style_pad_row(s_launcher, 8, 0);
    /* Don't let a stray touch scroll the icon grid out of view (the 4×3 grid fits
     * 12 tiles on one screen). Re-enable for paging once there are more than fit. */
    lv_obj_remove_flag(s_launcher, LV_OBJ_FLAG_SCROLLABLE);

    /* Bottom drag-up zone on the top layer: a clickable patch at the bottom-CENTRE
     * only — same width as the home-indicator pill — above all program content.
     * Sitting on the top layer it intercepts any press that STARTS inside it, so a
     * drag begun here goes home even over scrollable program content; a press begun
     * above it OR to either side reaches that content normally (so the rest of the
     * bottom row — e.g. a chat compose field + Send — stays usable). The patch's
     * geometry is the condition, no per-event position test needed. It must not be
     * scrollable itself (a scrollable object would eat the drag). Keep the top layer
     * click-through so taps outside the patch reach the program. */
    int homebarH = lcdScreenH() / 10;
    int homebarW = 90;                       /* centre-only; matches the pill width below */
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_SCROLLABLE);
    s_homebar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_homebar);
    lv_obj_remove_flag(s_homebar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_homebar, homebarW, homebarH);
    lv_obj_set_pos(s_homebar, (lcdScreenW() - homebarW) / 2, lcdScreenH() - homebarH);
    lv_obj_add_flag(s_homebar, LV_OBJ_FLAG_CLICKABLE);
    /* PRESS_LOCK pins the whole press to the home-bar even as the finger slides up
     * off it — without it LVGL re-searches the object under the finger on every
     * move and reassigns the press to the content underneath, so the drag would
     * never stay here. The handlers (above) then track the drag by hand. */
    lv_obj_add_flag(s_homebar, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_homebar, homebarPressed,  LV_EVENT_PRESSED,    nullptr);
    lv_obj_add_event_cb(s_homebar, homebarPressing, LV_EVENT_PRESSING,   nullptr);
    lv_obj_add_event_cb(s_homebar, homebarReleased, LV_EVENT_RELEASED,   nullptr);
    lv_obj_add_event_cb(s_homebar, homebarReleased, LV_EVENT_PRESS_LOST, nullptr);

    /* Visible home-indicator pill so the bottom drag-up zone is discoverable.
     * Purely decorative: strip the default CLICKABLE/SCROLLABLE flags so a press
     * on the pill (dead centre, where users aim) falls through to the home-bar
     * instead of being swallowed by the pill — otherwise the drag only works on
     * the sides, clear of the pill. */
    lv_obj_t* pill = lv_obj_create(s_homebar);
    lv_obj_remove_style_all(pill);
    lv_obj_remove_flag(pill, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(pill, homebarW, 4);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_color(pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_40, 0);
    lv_obj_set_style_radius(pill, 2, 0);

    /* Full-width near-white hairline that rides the window's bottom edge, so the
     * app you're pulling up reads as distinct from the launcher background showing
     * beneath it. On the top layer (above program content); placeChrome() keeps it
     * pinned to the current layer's bottom. */
    s_bottomLine = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_bottomLine);
    lv_obj_remove_flag(s_bottomLine, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(s_bottomLine, lcdScreenW(), 1);
    lv_obj_set_pos(s_bottomLine, 0, lcdScreenH() - 1);
    lv_obj_set_style_bg_color(s_bottomLine, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(s_bottomLine, LV_OPA_COVER, 0);

    /* The home-bar + hairline belong to a shown program, not the launcher. Boot
     * lands on the launcher, so start both hidden; openEntry reveals the pill, and
     * a drag reveals the edge. */
    showHomebar(false); showLine(false);
}

void lcdLauncherAdd(const char* name, const char* basename, lcd_fn_t fn) {
    if (!s_launcher) return;
    size_t idx = s_entries.size();

    lv_obj_t* tile = lv_button_create(s_launcher);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, TILE_W, TILE_H);
    lv_obj_add_event_cb(tile, onTileClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), tile);

    /* Icon top, label bottom — absolute align (proven; avoids any flex quirk).
     * Rendered at native size: the icon bucket is sized to the tile (see
     * lcd_icons LAUNCHER_ICON_RES), so no runtime scaling is needed. */
    lv_obj_t* img = lv_image_create(tile);   /* src set when the icon loads */
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t* lbl = lv_label_create(tile);
    lv_label_set_text(lbl, name ? name : "");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);   /* compact: 4×3 grid */
#endif
    /* Clamp to tile width and ellipsize — long names mustn't wrap to a 2nd line
     * (the short tile has no room for it) or spill past the tile edge. */
    lv_obj_set_width(lbl, TILE_W);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -2);

    info("add tile '%s' (icon '%s')\n", name ? name : "", basename ? basename : "");

    Entry e;
    e.name     = name ? name : "";
    e.basename = basename ? basename : "";
    e.fn       = fn;
    e.img      = img;
    s_entries.push_back(std::move(e));

    lcdIconRequest(s_entries[idx].basename.c_str());   /* -> lcdLauncherIconLoaded */
}

void lcdLauncherIconLoaded(const char* basename) {
    if (!basename) return;
    char src[160];
    lcdIconSrc(basename, src, sizeof(src));
    for (auto& e : s_entries)
        if (e.img && e.basename == basename)
            lv_image_set_src(e.img, src);
}

void lcdLauncherReload(void) {
    for (auto& e : s_entries) {
        if (lcdIconReady(e.basename.c_str())) {
            char src[160];
            lcdIconSrc(e.basename.c_str(), src, sizeof(src));
            if (e.img) lv_image_set_src(e.img, src);
        } else {
            lcdIconRequest(e.basename.c_str());
        }
    }
}

void lcdGoHomeInternal(void) {
    if (!s_current) return;
    lv_obj_t* layer = s_current;
    int fromY = lv_obj_get_y(layer);      /* 0 if fullscreen, else LCD_STATUSBAR_H */
    s_current = nullptr;                  /* a second Home (or one at the launcher) is a no-op */
    showHomebar(false); showLine(false);  /* the window is leaving — drop its bar + edge */
    applyFullscreen();                    /* nothing shown -> restore the status bar */

    /* Slide the program up off the top, revealing the launcher beneath (a lower
     * sibling that's always present); homeSlideDone hides + re-parks it at the end. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, layer);
    lv_anim_set_exec_cb(&a, [](void* v, int32_t y) { lv_obj_set_y((lv_obj_t*)v, y); });
    lv_anim_set_values(&a, fromY, -lcdScreenH());
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, homeSlideDone);
    lv_anim_start(&a);
}

void lcdProgramFullscreen(bool on) {
    s_fullscreenLayer = on ? s_current : nullptr;
    applyFullscreen();
}

/* Marks the current layer (the one being built / shown) as wanting trackball
 * arrows; the flag rides the layer so it persists across Home/re-open, and
 * applyFullscreen() turns the mode on only while that layer is current. */
void lcdProgramScrollwheelArrows(bool on) {
    if (s_current) {
        if (on) lv_obj_add_flag(s_current, LV_OBJ_FLAG_USER_1);
        else    lv_obj_remove_flag(s_current, LV_OBJ_FLAG_USER_1);
    }
    applyFullscreen();
}
