/**
 * lcd_app.cpp — LcdApp base implementation: the install registry, the per-app
 * resource ledger, and the service methods that delegate to the manager.
 *
 * The ledger is the enable_recycle_resource replacement (plan §4.3): timers and
 * anims created through app.timer()/app.anim() are recorded and freed when the
 * shell evicts the app — no walking of LVGL's private lists. Objects need no
 * ledger; deleting root() frees the whole tree.
 */
#include "lcd_app.h"
#include "shell_internal.h"
#include "lcd_internal.h"   /* lcdInputGroup */

#include <vector>

namespace {
std::vector<LcdApp*> s_apps;   /* every installed app; the shell owns these */
}

int lcdInstall(LcdApp* app) {
    if (!app) return -1;
    int id = (int)s_apps.size();
    app->_setId(id);
    s_apps.push_back(app);
    shellLauncherAddTile(app);
    return id;
}

LcdApp* shellFindApp(const char* name) {
    if (!name) return nullptr;
    for (auto* a : s_apps)
        if (a->cfg().name && std::string(a->cfg().name) == name) return a;
    return nullptr;
}

const std::vector<LcdApp*>& shellApps(void) { return s_apps; }

/* ---- services ---- */

lv_group_t* LcdApp::inputGroup()           { return lcdInputGroup(); }
void        LcdApp::goHome()               { shellNavigate(NavIntent::HOME); }
void        LcdApp::stop()                 { shellStopApp(this); }
void        LcdApp::setFullscreen(bool on) { m_fullscreen = on; shellAppChanged(this); }
void        LcdApp::setScrollwheelArrows(bool on) { m_arrows = on; shellAppChanged(this); }
void        LcdApp::setScrollHandler(lcd_scroll_fn_t fn) { m_scrollFn = fn; }

void LcdApp::setStatusIcon(int /*area*/, const char* /*iconBasename*/, int /*state*/) {
    /* M2 wires the status-bar app-icon renderer (plan §5.3). Stub until then. */
}
void LcdApp::setRecentsSubtitle(const char* s) { m_recentsSubtitle = s ? s : ""; }

/* ---- recents thumbnail ---- */

void LcdApp::_freeThumb() {
    if (m_thumb) { lv_draw_buf_destroy(m_thumb); m_thumb = nullptr; }
}

void LcdApp::_captureThumb() {
#if LV_USE_SNAPSHOT
    if (!m_root) return;
    /* Paint the live app tree into a fresh PSRAM buffer (lv_mem_spangap routes
     * the alloc to PSRAM). Must run while the root is drawn and opaque — the
     * manager calls this just before it hides/fades the app. RGB565 matches the
     * panel, so the card draws with no format conversion. */
    _freeThumb();
    lv_draw_buf_t* full = lv_snapshot_take(m_root, LV_COLOR_FORMAT_RGB565);
    if (!full) return;
#if LV_USE_CANVAS
    /* Downscale to half size into a canvas so the RESIDENT thumbnail is a quarter
     * of the screen's pixels (the card shows it ~1:1). The full snapshot is just
     * scratch — drawn through, then freed; only the miniature is kept. */
    int32_t w = full->header.w, h = full->header.h;
    lv_draw_buf_t* mini = lv_draw_buf_create(w / 2, h / 2, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (mini) {
        lv_obj_t* canvas = lv_canvas_create(lv_layer_top());
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        lv_canvas_set_draw_buf(canvas, mini);
        lv_layer_t layer;
        lv_canvas_init_layer(canvas, &layer);
        lv_draw_image_dsc_t dsc;
        lv_draw_image_dsc_init(&dsc);
        dsc.src     = full;
        dsc.scale_x = 128;   /* 256 = 1:1, so 128 = half */
        dsc.scale_y = 128;
        lv_area_t coords = { 0, 0, w - 1, h - 1 };  /* natural size; scale shrinks it */
        lv_draw_image(&layer, &dsc, &coords);
        lv_canvas_finish_layer(canvas, &layer);
        lv_obj_delete(canvas);              /* keeps mini — canvas never owns it */
        lv_draw_buf_destroy(full);
        m_thumb = mini;
        return;
    }
#endif
    /* No canvas (or the mini alloc failed): keep the full-size snapshot. */
    m_thumb = full;
#endif
}

/* ---- resource ledger ---- */

lv_timer_t* LcdApp::timer(lv_timer_cb_t cb, uint32_t period_ms, void* user) {
    lv_timer_t* t = lv_timer_create(cb, period_ms, user);
    if (t) m_timers.push_back(t);
    return t;
}

lv_anim_t* LcdApp::anim() {
    m_animScratch = lv_anim_t{};
    lv_anim_init(&m_animScratch);
    return &m_animScratch;
}

void LcdApp::startAnim(lv_anim_t* a) {
    if (!a) return;
    m_anims.push_back({ a->var, a->exec_cb });   /* record before LVGL copies it in */
    lv_anim_start(a);
}

void LcdApp::_reclaimLedger() {
    for (auto* t : m_timers) lv_timer_delete(t);
    for (auto& a : m_anims)  lv_anim_delete(a.var, a.exec_cb);
    m_timers.clear();
    m_anims.clear();
    _freeThumb();   /* the snapshot belonged to the now-deleted root */
}
