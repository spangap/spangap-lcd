/**
 * lcd_app.h — the on-device app object model for the phone-style shell.
 *
 * An LcdApp is one launcher program with a real lifecycle. It replaces the old
 * lcdRegister(name, icon, fn) + scattered-free-function model: instead of a
 * build-once function pointer and a pile of lcdProgram* setters keyed off "the
 * currently shown layer", an app is an object that owns exactly its root layer
 * and overrides the lifecycle it cares about.
 *
 * Threading is unchanged from the launcher: EVERY method here runs on the lcd
 * task (LVGL is single-threaded). Build/show/hide/back/close are all called by
 * the shell at the same points the legacy launcher called its fn / showFn /
 * eviction. Cross-task work still hops on with lcdRun()/ON_LCD (lcd.h).
 *
 * Only compiled-meaningful when CONFIG_LCD_PHONE=y (the new shell). The legacy
 * launcher does not use this header. Available alongside lcd.h, which keeps the
 * retained display / input / text-view / settings surface.
 */
#ifndef SPANGAP_LCD_APP_H
#define SPANGAP_LCD_APP_H

#include "lvgl.h"
#include "lcd.h"      /* lcd_scroll_fn_t, fonts, text view, lcdRun/ON_LCD */
#include "service.h"  /* Service base: an LcdApp is a boot-registered service */

#include <vector>
#include <string>
#include <cstdint>

/** A navigation request, decoupled from its producer (gesture bar, ESC key,
 *  board Home button, on-screen nav bar). The shell's single consumer is
 *  shellNavigate(). */
enum class NavIntent { BACK, HOME, RECENTS };

/** Base class for an on-device app. An LcdApp IS a Service (service.h): declare
 *  it in a straddle's `services:` list and the generated boot registration
 *  constructs it and the onInit below installs its launcher tile — no
 *  lcdInstall() hook to hand-wire. (A shell-constructed built-in — Settings,
 *  the launcher — is an LcdApp that simply never registers: its boot virtuals
 *  never fire, and shellInit still calls lcdInstall() directly for the id.)
 *  Subclass it, override the UI lifecycle, put boot-task wiring in appInit().
 *  The shell owns the instance thereafter (never deletes it — only its root
 *  layer is evictable). */
class LcdApp : public Service {
public:
    struct Config {
        const char* name         = "";    /* launcher label + lookup key (string literal) */
        const char* iconBasename = "";    /* /fixed/lcd/icons/<res>/<name>.bin (string literal) */
        bool        statusBar    = true;  /* keep the status bar while shown */
        bool        navBar       = false; /* request the on-screen Back/Home/Recents bar */
        bool        fullscreen   = false; /* immersive: reclaim the status-bar rows on show */
        uint8_t     launcherPage = 0;     /* which launcher page the tile lands on */
    };

    explicit LcdApp(const Config& cfg) : m_cfg(cfg) {}
    virtual ~LcdApp() {}

    /* ---- Lifecycle (all on the lcd task) ----
     * Maps to the legacy launcher as: onCreate <- Entry::fn (build once, lazily,
     * on first open), onShow <- Entry::showFn (every open after build), onHide <-
     * the hide path when another layer becomes current, onClose <- the
     * LV_EVENT_DELETE path under eviction. onBack is new (there was no Back). */
    virtual void onCreate(lv_obj_t* root) = 0; /* build UI once, into root */
    virtual void onShow() {}                    /* brought to foreground */
    virtual void onHide() {}                     /* sent to background */
    virtual bool onBack() { return false; }      /* true = handled; false = go Home */
    virtual void onClose() {}                     /* stopped or evicted; free the app's
                                                    non-LVGL resources (close connections,
                                                    drop handles). Ledger auto-freed after;
                                                    the root layer is deleted after this. */

    /* ---- Boot lifecycle (Service; on the boot task) ----
     * onInit is the ONE thing a registered LcdApp does at boot: hop onto the lcd
     * task and install its launcher tile. It is `final` — a registered LcdApp
     * always gets its tile, so forgetting a base call can't silently drop it.
     * The platform band brings the lcd task up before any straddle-band service's
     * onInit runs, so the hop always has a live task. App-specific boot wiring
     * (CLI verbs, mutexes, worker tasks) goes in appInit(), which runs on the
     * boot task right after — the safe place for cliRegister/spawnTask.
     * Defined out-of-line below (needs lcdInstall, declared after the class). */
    void onInit() final;

    /* ---- Services the app calls (replace the old free functions) ----
     * Valid from onCreate onward (root exists). They operate on THIS app, not on
     * "the currently shown layer", so there is no ordering trap. */
    lv_obj_t*   root()        { return m_root; }
    lv_group_t* inputGroup();                        /* was lcdInputGroup() */
    void        goHome();                            /* was lcdGoHome() */
    /** Stop this app: run onClose() teardown, free its root layer + ledger, drop
     *  it from the running set, and — if it was in the foreground — hand the
     *  screen back to the launcher. The single termination entry point: the
     *  recents swipe-kill calls it, and an app calls it on itself to bail out of
     *  the foreground when it can no longer function (e.g. its connection
     *  dropped), so it never sits dead in limbo. A no-op if not built. Lcd task. */
    void        stop();
    void        setFullscreen(bool on);              /* was lcdProgramFullscreen() */
    void        setScrollwheelArrows(bool on);       /* was lcdProgramScrollwheelArrows() */
    void        setScrollHandler(lcd_scroll_fn_t fn); /* was lcdProgramScrollHandler() */
    /** Push a status-bar app icon (3 alignment areas, multi-state). M2 wires the
     *  renderer; a stub until then. `area` 0..2, `state` app-defined. */
    void        setStatusIcon(int area, const char* iconBasename, int state);
    /** One line shown on this app's recents card (e.g. current contact). */
    void        setRecentsSubtitle(const char* s);

    /* ---- Resource ledger (the enable_recycle_resource replacement) ----
     * Timers and animations live in LVGL globals with no owner, so a closed app
     * would leak them. Create them through these and the shell frees them when
     * the app's root is evicted (onClose) — no LVGL-internals walking. Objects
     * need no ledger: deleting root() frees the whole tree. */
    lv_timer_t* timer(lv_timer_cb_t cb, uint32_t period_ms, void* user = nullptr);
    /** A zeroed, tracked lv_anim_t scratch — init/configure it, then startAnim().
     *  The shell records {var, exec_cb} so it can lv_anim_delete() on close. */
    lv_anim_t*  anim();
    void        startAnim(lv_anim_t* a);

    /* ---- Shell-internal accessors (used by manager/launcher; not app API) ---- */
    const Config& cfg() const { return m_cfg; }
    int           id() const  { return m_id; }
    void          _setId(int id)        { m_id = id; }
    void          _setRoot(lv_obj_t* r) { m_root = r; }
    const std::string& _recentsSubtitle() const { return m_recentsSubtitle; }
    /** Recents thumbnail: a PSRAM snapshot of this app's screen, captured by the
     *  manager the moment the app leaves the foreground (while still drawn) and
     *  shown half-scale on its recents card. Null until first captured (the card
     *  then falls back to the launcher icon). Freed on eviction. */
    lv_draw_buf_t* _thumb() const { return m_thumb; }
    void           _captureThumb();   /* snapshot m_root -> m_thumb (replaces prior) */
    void           _freeThumb();
    /* UI state that rides the app across hide/show (as the legacy flags rode the
     * layer). The manager reads these in applyChrome() while the app is shown. */
    bool            _fullscreen() const { return m_fullscreen; }
    bool            _arrows() const     { return m_arrows; }
    lcd_scroll_fn_t _scrollFn() const   { return m_scrollFn; }
    /* Keypad focus to restore when this app is re-shown (per-app, as the legacy
     * launcher kept per-entry — stops a hidden app stranding the shared group). */
    lv_obj_t*       _savedFocus() const         { return m_savedFocus; }
    void            _setSavedFocus(lv_obj_t* o) { m_savedFocus = o; }
    /** Free every ledgered timer/anim. Called by the shell after onClose(). */
    void          _reclaimLedger();

protected:
    /** Boot-task wiring, run once from onInit() after the tile is installed:
     *  CLI verbs, mutex creation, worker-task spawn, storage subscriptions.
     *  Runs on the boot task (not the lcd task), so cliRegister/spawnTask are
     *  safe here. Default no-op — an app with no boot wiring omits it. Non-LCD
     *  init decoupled from the app belongs in a separate plain Service. */
    virtual void appInit() {}

private:
    Config          m_cfg;
    lv_obj_t*       m_root = nullptr;
    int             m_id   = -1;
    std::string     m_recentsSubtitle;
    bool            m_fullscreen = false;
    bool            m_arrows     = false;
    lcd_scroll_fn_t m_scrollFn   = nullptr;
    lv_obj_t*       m_savedFocus = nullptr;
    lv_draw_buf_t*  m_thumb      = nullptr;   /* recents thumbnail (PSRAM snapshot) */

    struct AnimRef { void* var; lv_anim_exec_xcb_t exec_cb; };
    std::vector<lv_timer_t*> m_timers;
    std::vector<AnimRef>     m_anims;
    lv_anim_t                m_animScratch{};   /* filled by anim(), copied on start */
};

/** Install an app: adds its launcher tile and records it. Returns its id (also
 *  available as app->id()). The shell builds the root lazily on first open
 *  (onCreate), keeps it resident, and may evict (onClose -> free root + ledger)
 *  under memory pressure, rebuilding on next open. The instance is owned by the
 *  shell from here on. Call on the lcd task (e.g. from a *LcdRegister hook hopped
 *  on with lcdRun, or from shellInit). */
int lcdInstall(LcdApp* app);

/** The boot hop: install this app's tile on the lcd task, then run its
 *  boot-task wiring. Out-of-line so it can name lcdInstall (declared just
 *  above). */
inline void LcdApp::onInit() {
    lcdRun([](void* a) { lcdInstall(static_cast<LcdApp*>(a)); }, this);
    appInit();
}

#endif /* SPANGAP_LCD_APP_H */
