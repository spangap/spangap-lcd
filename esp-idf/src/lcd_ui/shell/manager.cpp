/**
 * manager.cpp — the phone shell's foreground/back/home state machine, plus the
 * home-bar gesture chrome and the retained legacy public surface (lcd.h) that
 * unconverted straddles still call.
 *
 * Stack-free model (like the donor's MAIN/APP/RECENTS screens): the launcher is
 * the root, apps are peers, one app foreground at a time. "Running" apps (the
 * recents set, M3) are exactly the apps whose root layer currently exists —
 * onCreate called, onClose/eviction not yet — so the resident layer IS the
 * membership, no separate bookkeeping. This mirrors the legacy launcher's
 * openEntry/lcdGoHomeInternal layer lifecycle exactly; only the typed LcdApp
 * replaces the Entry/function-pointer.
 *
 * The program layer's user_data holds the owning LcdApp* (non-null marks a
 * program layer vs the launcher grid), so a layer DELETE can null the app's root
 * and clear the foreground without any stored back-pointer to dangle.
 */
#include "shell_internal.h"
#include "stylesheet.h"
#include "lcd_internal.h"   /* lcdScreenW/H, lcdInputGroup, lcdScrollwheelArrowsApply,
                               lcdStatusbarSetVisible, lcdStatusbarInit */
#include "log.h"

namespace {

lv_obj_t*   s_screenObj   = nullptr;
LcdApp*     s_foreground  = nullptr;       /* null <=> at the launcher */
ShellScreen s_screen      = ShellScreen::LAUNCHER;

/* Home-bar drag chrome (on lv_layer_top; ported from the legacy launcher). */
lv_obj_t* s_homebar    = nullptr;
lv_obj_t* s_bottomLine = nullptr;
int      s_dragStartY = 0;
int      s_dragBaseY  = 0;
bool     s_dragging   = false;

LcdApp* appOfLayer(lv_obj_t* layer) {
    return layer ? static_cast<LcdApp*>(lv_obj_get_user_data(layer)) : nullptr;
}

/* ---- chrome geometry ---- */

void setHidden(lv_obj_t* o, bool hidden) {
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
void showHomebar(bool on) { setHidden(s_homebar, !on); }
void showLine(bool on)    { setHidden(s_bottomLine, !on); }

/* The home-bar + hairline ride the bottom of the foreground layer as it's
 * dragged up. Place them off screenH + dy (dy<=0); never read the layer coords
 * (stale until the next layout). */
void placeChrome(int dy = 0) {
    int barH = lcdScreenH() / 10;
    if (s_homebar)    lv_obj_set_y(s_homebar,    lcdScreenH() - barH + dy);
    if (s_bottomLine) lv_obj_set_y(s_bottomLine, lcdScreenH() - 1   + dy);
}

/* Status bar visibility + foreground geometry + trackball-arrows all follow the
 * foreground app's flags. Re-run on every open/home and whenever an app flips a
 * flag (shellAppChanged). */
void applyChrome() {
    bool fs       = s_foreground && s_foreground->_fullscreen();
    bool barShown = !fs && (!s_foreground || s_foreground->cfg().statusBar);
    lcdStatusbarSetVisible(barShown);
    int top = barShown ? lcdStyle().statusBar.h : 0;
    if (s_foreground && s_foreground->root()) {
        lv_obj_set_pos(s_foreground->root(), 0, top);
        lv_obj_set_size(s_foreground->root(), lcdScreenW(), lcdScreenH() - top);
    }
    lcdScrollwheelArrowsApply(s_foreground && s_foreground->_arrows());
}

/* ---- per-app keypad focus (save on leave, restore on return) ---- */

void saveFocus(LcdApp* app) {
    if (!app || !app->root() || !lcdInputGroup()) return;
    app->_setSavedFocus(nullptr);
    lv_obj_t* f = lv_group_get_focused(lcdInputGroup());
    for (lv_obj_t* p = f; p; p = lv_obj_get_parent(p))
        if (p == app->root()) { app->_setSavedFocus(f); break; }
}
void restoreFocus(LcdApp* app) {
    if (!app || !lcdInputGroup()) return;
    lv_obj_t* f = app->_savedFocus();
    if (f && lv_obj_is_valid(f)) lv_group_focus_obj(f);
}

/* ---- opacity fades / launcher reveal ----
 * The launcher sits hidden behind a lifted app (so a half-swipe reveals only the
 * dark backdrop, never the launcher); it fades in over 200ms only when the swipe
 * commits to Home (>60%). lv_style_opa fades an object AND its whole subtree. */
const uint32_t kFadeMs = 200;

void fadeOpa(lv_obj_t* o, lv_opa_t from, lv_opa_t to) {
    if (!o) return;
    lv_anim_delete(o, nullptr);
    lv_obj_set_style_opa(o, from, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, [](void* v, int32_t op) { lv_obj_set_style_opa((lv_obj_t*)v, (lv_opa_t)op, 0); });
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, kFadeMs);
    lv_anim_start(&a);
}
void revealLauncher(bool animate) {
    lv_obj_t* L = shellLauncherRoot();
    if (!L) return;
    lv_obj_remove_flag(L, LV_OBJ_FLAG_HIDDEN);
    if (animate) fadeOpa(L, LV_OPA_TRANSP, LV_OPA_COVER);
    else         lv_obj_set_style_opa(L, LV_OPA_COVER, 0);
}
void hideLauncher() {
    lv_obj_t* L = shellLauncherRoot();
    if (L) lv_obj_add_flag(L, LV_OBJ_FLAG_HIDDEN);
}

/* ---- program layer lifecycle ---- */

void onLayerDelete(lv_event_t* e) {
    lv_obj_t* t   = (lv_obj_t*)lv_event_get_target(e);
    LcdApp*   app = appOfLayer(t);
    if (app) { app->_setRoot(nullptr); app->_setSavedFocus(nullptr); }
    if (app == s_foreground) s_foreground = nullptr;
}

lv_obj_t* makeProgramLayer(LcdApp* app) {
    const LcdStyle& st = lcdStyle();
    lv_obj_t* L = lv_obj_create(s_screenObj);
    lv_obj_remove_style_all(L);
    lv_obj_set_pos(L, 0, st.statusBar.h);
    lv_obj_set_size(L, lcdScreenW(), lcdScreenH() - st.statusBar.h);
    lv_obj_set_style_bg_color(L, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(L, LV_OPA_COVER, 0);
    lv_obj_set_user_data(L, app);
    lv_obj_add_event_cb(L, onLayerDelete, LV_EVENT_DELETE, nullptr);
    return L;
}

/* Home slide-up: hide + re-park the layer once the animation completes, and fire
 * the app's onHide() (it just went to the background). */
void homeSlideDone(lv_anim_t* a) {
    lv_obj_t* layer = (lv_obj_t*)a->var;
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(layer, lcdStyle().statusBar.h);
    showHomebar(false); showLine(false);
    if (LcdApp* app = appOfLayer(layer)) app->onHide();
}

/* ---- home-bar drag (manual; LVGL gestures are unreliable on a short strip) ---- */

int activePointerY() {
    lv_indev_t* ind = lv_indev_active();
    if (!ind) return 0;
    lv_point_t p;
    lv_indev_get_point(ind, &p);
    return p.y;
}

void homebarPressed(lv_event_t*) {
    s_dragging = false;
    if (!s_foreground || !s_foreground->root()) return;
    lv_anim_delete(s_foreground->root(), nullptr);
    s_dragStartY = activePointerY();
    s_dragBaseY  = lv_obj_get_y(s_foreground->root());
    s_dragging   = true;
    showLine(true);
    placeChrome();
}

void homebarPressing(lv_event_t*) {
    if (!s_dragging || !s_foreground || !s_foreground->root()) return;
    int dy = activePointerY() - s_dragStartY;
    if (dy > 0) dy = 0;
    lv_obj_set_y(s_foreground->root(), s_dragBaseY + dy);
    placeChrome(dy);
}

/* Spring the lifted app back down to its resting position (gesture cancelled). */
void springBack() {
    lv_obj_t* layer = s_foreground->root();
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, layer);
    lv_anim_set_exec_cb(&a, [](void* v, int32_t y) {
        lv_obj_set_y((lv_obj_t*)v, y); placeChrome((int)(y - s_dragBaseY)); });
    lv_anim_set_values(&a, lv_obj_get_y(layer), s_dragBaseY);
    lv_anim_set_duration(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void homebarReleased(lv_event_t* e) {
    if (!s_dragging || !s_foreground || !s_foreground->root()) {
        s_dragging = false; showLine(false); return;
    }
    s_dragging = false;
    showLine(false);
    /* Decide by how far the app was swiped up at release, as a fraction of the
     * screen height: >60% = HOME (launcher fades in), 20–60% = RECENTS (recents
     * fades in while the app fades out), <20% = cancel (spring back). A lost
     * press (not a real release) always cancels. The launcher stays hidden behind
     * the lifted app throughout — only a committed Home reveals it. */
    bool realRelease = (lv_event_get_code(e) == LV_EVENT_RELEASED);
    int  disp = s_dragBaseY - lv_obj_get_y(s_foreground->root());   /* px lifted (>=0) */
    float frac = (float)disp / (float)lcdScreenH();
    if (realRelease && frac >= 0.60f)      shellNavigate(NavIntent::HOME);
    else if (realRelease && frac >= 0.20f) shellNavigate(NavIntent::RECENTS);
    else                                   springBack();
}

void initChrome() {
    /* Bottom drag-up zone on the top layer: a centre-only clickable patch, above
     * all program content. PRESS_LOCK pins the press here as the finger slides
     * up; the handlers drive the drag by hand. (Ported from the legacy launcher.) */
    int barH = lcdScreenH() / 10;
    int barW = 90;
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_SCROLLABLE);
    s_homebar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_homebar);
    lv_obj_remove_flag(s_homebar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_homebar, barW, barH);
    lv_obj_set_pos(s_homebar, (lcdScreenW() - barW) / 2, lcdScreenH() - barH);
    lv_obj_add_flag(s_homebar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_homebar, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_homebar, homebarPressed,  LV_EVENT_PRESSED,    nullptr);
    lv_obj_add_event_cb(s_homebar, homebarPressing, LV_EVENT_PRESSING,   nullptr);
    lv_obj_add_event_cb(s_homebar, homebarReleased, LV_EVENT_RELEASED,   nullptr);
    lv_obj_add_event_cb(s_homebar, homebarReleased, LV_EVENT_PRESS_LOST, nullptr);

    lv_obj_t* pill = lv_obj_create(s_homebar);
    lv_obj_remove_style_all(pill);
    lv_obj_remove_flag(pill, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(pill, barW, 4);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 2, 0);

    s_bottomLine = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_bottomLine);
    lv_obj_remove_flag(s_bottomLine, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(s_bottomLine, lcdScreenW(), 1);
    lv_obj_set_pos(s_bottomLine, 0, lcdScreenH() - 1);
    lv_obj_set_style_bg_color(s_bottomLine, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(s_bottomLine, LV_OPA_COVER, 0);

    showHomebar(false); showLine(false);   /* boot lands on the launcher */
}

/* ---- edge-pan helpers (ported verbatim from the legacy launcher) ---- */

bool canScrollDir(lv_obj_t* o, lcd_scroll_dir_t dir) {
    switch (dir) {
        case LCD_SCROLL_UP:    return lv_obj_get_scroll_top(o)    > 0;
        case LCD_SCROLL_DOWN:  return lv_obj_get_scroll_bottom(o) > 0;
        case LCD_SCROLL_LEFT:  return lv_obj_get_scroll_left(o)   > 0;
        case LCD_SCROLL_RIGHT: return lv_obj_get_scroll_right(o)  > 0;
    }
    return false;
}
lv_obj_t* findScrollable(lv_obj_t* root, lcd_scroll_dir_t dir) {
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (canScrollDir(root, dir)) return root;
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* hit = findScrollable(lv_obj_get_child(root, i), dir);
        if (hit) return hit;
    }
    return nullptr;
}

}  // namespace

/* ---- shell-internal API ---- */

void shellInit(lv_obj_t* screen) {
    s_screenObj = screen;
    lcdStyleBegin(lcdScreenW(), lcdScreenH());
    /* Dark screen backdrop: a half-swiped-up app reveals this (not the launcher),
     * which stays hidden behind a lifted app until a committed Home fades it in. */
    lv_obj_set_style_bg_color(screen, lv_color_hex(lcdStyle().launcher.bg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lcdStatusbarInit();        /* top: opaque status bar (shell renderer) */
    shellLauncherInit(screen); /* bottom: paged icon grid */
    initChrome();              /* home-bar gesture + hairline on lv_layer_top */
    shellNavInstall();         /* ESC -> Back top-level key hook */

    /* Built-in apps. Settings is hosted by the existing page-stack unchanged —
     * lcdSettingsInit() registers the gear through the lcdRegister bridge, so it
     * lands as a real LcdApp with zero edits to lcd_settings.cpp or any straddle
     * pane (plan §3, §8.1: "SettingsApp is a host, not a generator"). Log and CLI
     * are first-class LcdApps. Other straddles' programs install themselves via
     * the same bridge. */
    lcdSettingsInit();                 /* gear (Settings host) */
    lcdInstall(lcdMakeLogApp());       /* Log */
    lcdInstall(lcdMakeCliApp());       /* CLI */

    info("phone shell ready\n");
}

void shellApplyZoom(void) {
    /* Runtime UI zoom (s.lcd.scale). Recalibrate the sheet (new font sizes +
     * theme), then rebuild the launcher and restyle the status bar so their
     * geometry/fonts reflow crisply. New-size fonts are fresh cache entries, so
     * any still-open app layer keeps its (valid) old-size fonts until it is
     * next rebuilt — no dangling. report_style_change refreshes theme-inherited
     * text. Lcd task (storage change dispatch). */
    lcdStyleRecalibrate();
    shellLauncherRebuild();
    lcdStatusbarRestyle();
    lv_obj_report_style_change(NULL);
    info("ui zoom applied (%d%%)\n", (int)(lcdUiScale() * 100 + 0.5f));
}

void shellOpenApp(LcdApp* app) {
    if (!app) return;
    bool firstBuild = (app->root() == nullptr);
    lv_obj_t* layer = firstBuild ? makeProgramLayer(app) : app->root();
    if (firstBuild) app->_setRoot(layer);
    lv_anim_delete(layer, nullptr);            /* cancel an in-flight Home slide */

    LcdApp* prev = s_foreground;
    /* Foreground BEFORE onCreate so an app's setFullscreen/setScrollwheelArrows
     * during build bind to itself (mirrors the legacy s_current-first order). */
    s_foreground = app;
    s_screen     = ShellScreen::APP;
    if (firstBuild) app->onCreate(layer);

    if (prev && prev != app && prev->root()) {
        prev->_captureThumb();   /* snapshot for recents while still drawn */
        saveFocus(prev);
        lv_obj_add_flag(prev->root(), LV_OBJ_FLAG_HIDDEN);
        prev->onHide();
    }
    applyChrome();
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(layer, LV_OPA_COVER, 0);   /* in case a prior recents faded it out */
    restoreFocus(app);
    showHomebar(true);
    placeChrome();
    hideLauncher();          /* launcher stays hidden behind the foreground app */
    app->onShow();
}

void shellNavigate(NavIntent intent) {
    /* Recents is a modal overlay over the foreground app: BACK/HOME dismiss it
     * back to that app (or the launcher if none) without disturbing it. */
    if (shellRecentsVisible() && intent != NavIntent::RECENTS) {
        shellRecentsHide();
        if (s_foreground && s_foreground->root()) {
            if (intent == NavIntent::HOME) { shellNavigate(NavIntent::HOME); return; }
            /* BACK: restore the app underneath (it was faded out for recents). */
            s_screen = ShellScreen::APP;
            lv_obj_set_style_opa(s_foreground->root(), LV_OPA_COVER, 0);
            applyChrome();
            showHomebar(true); placeChrome();
        } else {
            s_screen = ShellScreen::LAUNCHER;
            revealLauncher(true);
        }
        return;
    }
    switch (intent) {
        case NavIntent::HOME: {
            if (!s_foreground) { s_screen = ShellScreen::LAUNCHER; return; }
            LcdApp* app = s_foreground;
            app->_captureThumb();   /* snapshot for recents before the slide-up */
            saveFocus(app);
            int fromY = lv_obj_get_y(app->root());   /* 0 if fullscreen, else statusBar.h */
            s_foreground = nullptr;
            s_screen     = ShellScreen::LAUNCHER;
            showHomebar(false); showLine(false);
            applyChrome();                            /* nothing shown -> restore the bar */
            revealLauncher(true);                     /* fade the launcher in over 200ms */
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, app->root());
            lv_anim_set_exec_cb(&a, [](void* v, int32_t y) { lv_obj_set_y((lv_obj_t*)v, y); });
            lv_anim_set_values(&a, fromY, -lcdScreenH());
            lv_anim_set_duration(&a, 220);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
            lv_anim_set_completed_cb(&a, homeSlideDone);
            lv_anim_start(&a);
            return;
        }
        case NavIntent::BACK:
            if (s_foreground) { if (!s_foreground->onBack()) shellNavigate(NavIntent::HOME); }
            return;
        case NavIntent::RECENTS:
            /* Fade the (possibly mid-swipe) foreground app out while recents fades
             * in, both over 200ms. The app keeps running underneath; BACK restores
             * its opacity + position. The launcher stays hidden. */
            if (s_foreground && s_foreground->root()) {
                s_foreground->_captureThumb();   /* snapshot before fading out */
                fadeOpa(s_foreground->root(), LV_OPA_COVER, LV_OPA_TRANSP);
            }
            showHomebar(false); showLine(false);
            s_screen = ShellScreen::RECENTS;
            shellRecentsShow();   /* fades in over 200ms */
            return;
    }
}

LcdApp*     shellForeground(void) { return s_foreground; }
ShellScreen shellScreen(void)     { return s_screen; }
void        shellAppChanged(LcdApp* app) { if (app == s_foreground) applyChrome(); }

void shellStopApp(LcdApp* app) {
    if (!app || !app->root()) return;
    bool wasFg = (app == s_foreground);
    lv_obj_t* root = app->root();
    app->onClose();        /* app teardown: close connections, drop handles */
    app->_reclaimLedger();
    lv_obj_delete(root);   /* onLayerDelete nulls app->root() + clears s_foreground */
    /* A stopped foreground app hands the screen back to the launcher, revealed —
     * it was hidden behind the lifted app — unless the recents switcher is up
     * over it (it stays, having just dropped this card). */
    if (wasFg && !shellRecentsVisible()) {
        s_screen = ShellScreen::LAUNCHER;
        showHomebar(false); showLine(false);
        revealLauncher(false);
        applyChrome();
    }
}

/* Memory-pressure eviction is the same teardown as an explicit stop. */
void shellEvictApp(LcdApp* app) { shellStopApp(app); }

/* ---- retained legacy public surface (lcd.h) — bridges the unconverted apps ---- */

bool lcdAtLauncher(void) { return s_foreground == nullptr; }

void lcdShowProgram(const char* name) {
    if (LcdApp* a = shellFindApp(name)) shellOpenApp(a);
}

void lcdGoHomeInternal(void) { shellNavigate(NavIntent::HOME); }

void lcdProgramFullscreen(bool on) {
    if (s_foreground) s_foreground->setFullscreen(on);
}
void lcdProgramScrollwheelArrows(bool on) {
    if (s_foreground) s_foreground->setScrollwheelArrows(on);
}
void lcdProgramScrollHandler(lcd_scroll_fn_t fn) {
    if (s_foreground) s_foreground->setScrollHandler(fn);
}

void lcdScroll(lcd_scroll_dir_t dir, int amount) {
    if (amount <= 0) return;
    int dx = 0, dy = 0;
    switch (dir) {
        case LCD_SCROLL_UP:    dy =  amount; break;
        case LCD_SCROLL_DOWN:  dy = -amount; break;
        case LCD_SCROLL_LEFT:  dx =  amount; break;
        case LCD_SCROLL_RIGHT: dx = -amount; break;
    }
    if (s_foreground) {
        if (lcd_scroll_fn_t fn = s_foreground->_scrollFn()) { fn(dx, dy); return; }
    }
    lv_obj_t* root = s_foreground ? s_foreground->root() : shellLauncherRoot();
    lv_obj_t* o    = findScrollable(root, dir);
    if (o) lv_obj_scroll_by_bounded(o, dx, dy, LV_ANIM_OFF);
}
