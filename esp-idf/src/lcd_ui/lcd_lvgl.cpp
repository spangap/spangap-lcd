/**
 * lcd_lvgl.cpp — LVGL v9 bring-up over esp_lcd.
 *
 * Brings up the panel (lcd_panel.cpp, from Kconfig), creates the LVGL display
 * with a partial double buffer, wires the flush + DMA-done + tick callbacks, and
 * — for whichever input the board registered (lcd_input.h) — touch / cursor /
 * button indevs. A focus group is created for keypad indevs so a trackball- or
 * keyboard-only board can drive the same UI.
 */
#include "lcd_internal.h"
#include "lcd_input.h"

#include "log.h"
#include "spi_helper.h"   /* flushCb holds the shared-bus lock across the DMA drain */
#include "storage.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include <algorithm>          /* std::clamp */
#include <cstdint>

/* Draw-strip byte budget. Each flush is one SPI transfer, so a strip must fit
 * the shared-bus max_transfer_sz — the SD driver brings the bus up at 4096
 * (fs.cpp) before us, and spiHelperInitBus is first-caller-wins. Strip line
 * count is derived from this and the panel width so it holds for any size.
 * Two strips are double-buffered in internal DMA-capable RAM. */
#define LCD_DRAW_BUDGET 4000
#define LCD_TICK_MS     2

static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_display_t*          s_disp  = nullptr;
static lv_indev_t*            s_indev = nullptr;       /* touch pointer indev */
static lv_indev_t*            s_btnIndev = nullptr;   /* hardware Home/centre button */
static lv_group_t*            s_group = nullptr;
static int                    s_w = 0, s_h = 0;
static SemaphoreHandle_t      s_dmaDone = nullptr;   /* given when a strip's DMA completes */

/* Event-mode input bookkeeping (all touched only on the lcd task). The indevs
 * are LV_INDEV_MODE_EVENT, so they only read when lcdInputPoll() drives them. */
static lv_timer_t*            s_touchTrack = nullptr; /* 10ms re-read while a finger is down */
static bool                   s_inputAgain = false;   /* a read cb wants an immediate follow-up read */

/* Multi-touch / gestures (off by default — single-touch is the cheapest path).
 * When on, touchReadCb reads all fingers and dispatches to gesture handlers;
 * >=2 fingers suppress the single pointer so the gesture owner has the gesture. */
#define LCD_TOUCH_MAXPTS 5
static volatile bool          s_multipoint = false;
static lcd_gesture_cb_t       s_gestureCb[4] = {};

/* Trackball/mouse pointer (input->pointer_read). The board owns the position; we
 * own the LVGL pointer indev, the cursor object, and its show-on-move/auto-hide. */
static lv_indev_t*            s_ptrIndev   = nullptr;
static lv_obj_t*              s_cursor     = nullptr;
#define LCD_CURSOR_SIZE        14                            /* px; square cursor side */
static lv_timer_t*            s_ptrHide    = nullptr; /* one-shot: hide the cursor after inactivity */
static int                    s_ptrVisMs   = 2000;    /* <0 = always visible */

/* Screen-mirror remote pointer. A dedicated always-present pointer indev (so the
 * mirror works on any board, trackball or not) fed from lcdMirrorInjectPointer via
 * a small queue drained by mirrorReadCb on the lcd task. No cursor — the remote
 * user sees their own OS pointer over the browser canvas. */
struct MirrorPt { int16_t x, y; uint8_t pressed; };
static lv_indev_t*            s_mirrorIndev = nullptr;
static QueueHandle_t          s_mirrorQ     = nullptr;
static MirrorPt               s_mirrorLast  = { 0, 0, 0 };  /* level-held between events */

/* Screen-mirror remote keyboard. A keypad indev joined to the focus group, fed
 * from lcdMirrorInjectKey — one keystroke enqueues a press then a release. */
struct MirrorKey { uint32_t key; uint8_t pressed; };
static lv_indev_t*            s_mirrorKeyIndev = nullptr;
static QueueHandle_t          s_mirrorKeyQ     = nullptr;
static void                   mirrorSchedule(void);   /* defined after the indevs */

/* Strip DMA completed (ISR context). Wakes the flush, which then drops the
 * shared-bus lock. */
static bool onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
                        void*) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_dmaDone, &hp);
    return hp == pdTRUE;
}

/* Screen-mirror sink (lcdMirrorAttach). Read on the lcd task in flushCb; a lone
 * pointer store, so a plain volatile is enough for the cross-task publish. */
static volatile lcd_mirror_sink_t s_sink = nullptr;

static void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    auto panel = static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    /* Mirror tap: hand the little-endian pixels to the sink BEFORE the swap — the
     * browser wants little-endian; tapping after would mirror mangled colours. The
     * sink must copy and return at once (px is reused after flush). */
    lcd_mirror_sink_t sink = s_sink;
    if (sink) sink(area, px);
    /* esp_lcd ST7789 wants big-endian RGB565; LVGL renders little-endian. */
    lv_draw_sw_rgb565_swap(px, (uint32_t)w * h);
    /* Hold the shared-bus lock across the WHOLE transfer including the async
     * DMA drain — esp_lcd releases the SPI driver's own bus lock the moment the
     * color DMA is queued, so without this a co-resident polling driver (LoRa
     * on the same SPI2 bus) can grab the bus mid-DMA and panic. */
    spiHelperBusLock();
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px);
    xSemaphoreTake(s_dmaDone, portMAX_DELAY);
    spiHelperBusUnlock();
    lv_display_flush_ready(disp);
}

/* Re-read the touch while a finger is down — the GT911 INT only guarantees the
 * first edge, so this 10ms one keeps tracking smooth, then deletes itself on
 * release (see touchReadCb). Runs on the lcd task inside lv_timer_handler. */
static void touchTrackCb(lv_timer_t*) { if (s_indev) lv_indev_read(s_indev); }

static void touchReadCb(lv_indev_t*, lv_indev_data_t* data) {
    const lcd_input_t* in = lcdInput();
    lcd_raw_pt_t raw[LCD_TOUCH_MAXPTS];
    int cnt = 0;
    int max = s_multipoint ? LCD_TOUCH_MAXPTS : 1;
    if (!in || !in->touch_read || !in->touch_read(raw, max, &cnt) || cnt < 0) cnt = 0;

    /* The board reports raw points in NATIVE panel coords; map each to display
     * coords with the same rotation/mirror the panel applies to the pixels. */
    int n = cnt > LCD_TOUCH_MAXPTS ? LCD_TOUCH_MAXPTS : cnt;
    lcd_touch_pt_t gp[LCD_TOUCH_MAXPTS];
    for (int i = 0; i < n; i++) {
        int x, y;
        lcdPanelOrientTouch(raw[i].x, raw[i].y, &x, &y);
        gp[i].x = (int16_t)x; gp[i].y = (int16_t)y;
    }

    /* Keep re-reading while any finger is down (the GT911 INT only guarantees the
     * first edge). */
    if (n > 0) {
        if (!s_touchTrack) s_touchTrack = lv_timer_create(touchTrackCb, 10, nullptr);
    } else {
        if (s_touchTrack) { lv_timer_delete(s_touchTrack); s_touchTrack = nullptr; }
    }

    /* Gesture dispatch (multipoint only). */
    if (s_multipoint)
        for (auto cb : s_gestureCb) if (cb) cb(gp, n);

    /* Single pointer for LVGL: point 0 drives it, but >=2 fingers (multipoint)
     * suppress it so the gesture owner has the interaction. */
    static bool wasDown = false;
    bool suppress = (s_multipoint && n >= 2);
    if (n > 0 && !suppress) {
        data->point.x = gp[0].x;
        data->point.y = gp[0].y;
        data->state   = LV_INDEV_STATE_PRESSED;
        if (!wasDown) dbg("touch -> (%d,%d)\n", gp[0].x, gp[0].y);
        /* Touch is activity. Re-arm every pressed read (not just the down edge):
         * a board that drives us off-task (lcdTouchPoll) bypasses the lcd loop's
         * s_inputPending activity poke, and re-arming per read also keeps a long
         * drag from blanking mid-gesture. lcdActivity is a cheap timer reset. */
        lcdActivity();
        wasDown = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        wasDown = false;
    }
}

void lcdTouchSetMultipoint(bool on) { s_multipoint = on; }

void lcdTouchAddGestureHandler(lcd_gesture_cb_t cb) {
    if (!cb) return;
    for (auto& h : s_gestureCb) if (!h) { h = cb; return; }
}

/* Hardware Home/centre button as a keypad indev. The board owns all timing: its
 * click_read() asserts a click only on a short press (never during the >=Nms hold
 * it turns into lcdGoHome() itself). We turn that single asserted poll into an
 * ENTER press and force one follow-up read (s_inputAgain) so the release lands the
 * next poll — LVGL sees press+release = a click. */
static void buttonReadCb(lv_indev_t*, lv_indev_data_t* data) {
    const lcd_input_t* in = lcdInput();
    bool click = in && in->click_read && in->click_read();
    data->key = LV_KEY_ENTER;
    if (click) { data->state = LV_INDEV_STATE_PRESSED;  s_inputAgain = true; }
    else         data->state = LV_INDEV_STATE_RELEASED;
}

/* ---- trackball / mouse pointer ---- */

static void cursorHideCb(lv_timer_t*) {
    s_ptrHide = nullptr;
    if (s_cursor) lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
}

/* Show the cursor and (re)start the inactivity countdown. No-op when always-on. */
static void cursorPoke(void) {
    if (!s_cursor || s_ptrVisMs < 0) return;
    lv_obj_remove_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    if (s_ptrHide) { lv_timer_reset(s_ptrHide); return; }
    s_ptrHide = lv_timer_create(cursorHideCb, s_ptrVisMs > 0 ? (uint32_t)s_ptrVisMs : 1, nullptr);
    lv_timer_set_repeat_count(s_ptrHide, 1);
}

/* Glide the cursor toward (x,y) instead of teleporting. The trackball delivers one
 * read per pulse (EVENT mode); each retargets a short ease-out, so a flick trails
 * smoothly to rest. It's an lv_anim, so it self-drives the lcd loop frame-by-frame
 * (and rides the animation CPU boost) until it settles. snap=true jumps instantly —
 * used when the cursor first reappears after an auto-hide so it doesn't streak in
 * from wherever it last was. We own the position (no lv_indev_set_cursor). */
static void cursorGlideTo(int x, int y, bool snap) {
    if (!s_cursor) return;

    /* The board clamps the logical point (the click position, also its edge-pan
     * trigger) to the screen edge, but the cursor is drawn from its top-left, so
     * at the right/bottom edge the box would spill off. Clamp the *drawn* position
     * to [0, scr - size] to keep the whole cursor visible; the logical point the
     * caller passes on to LVGL is left at the edge. */
    int scrW = 0, scrH = 0;
    lcdDisplaySize(&scrW, &scrH);
    x = std::clamp(x, 0, (scrW > LCD_CURSOR_SIZE ? scrW : LCD_CURSOR_SIZE) - LCD_CURSOR_SIZE);
    y = std::clamp(y, 0, (scrH > LCD_CURSOR_SIZE ? scrH : LCD_CURSOR_SIZE) - LCD_CURSOR_SIZE);

    if (snap) { lv_anim_delete(s_cursor, nullptr); lv_obj_set_pos(s_cursor, x, y); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cursor);
    lv_anim_set_duration(&a, 100);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void* o, int32_t v) { lv_obj_set_x((lv_obj_t*)o, v); });
    lv_anim_set_values(&a, lv_obj_get_x(s_cursor), x);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, [](void* o, int32_t v) { lv_obj_set_y((lv_obj_t*)o, v); });
    lv_anim_set_values(&a, lv_obj_get_y(s_cursor), y);
    lv_anim_start(&a);
}

static void pointerReadCb(lv_indev_t*, lv_indev_data_t* data) {
    const lcd_input_t* in = lcdInput();
    int x = 0, y = 0;
    if (in && in->pointer_read && in->pointer_read(&x, &y)) {   /* moved */
        bool wasHidden = s_cursor && lv_obj_has_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        cursorPoke();
        cursorGlideTo(x, y, wasHidden);    /* snap into view, then ease on later pulses */
    }
    data->point.x = x;
    data->point.y = y;

    /* The centre button is the cursor's click — same board-owned click_read() as
     * the keypad path, pressed at the current cursor position. */
    bool click = in && in->click_read && in->click_read();
    if (click) { cursorPoke(); data->state = LV_INDEV_STATE_PRESSED; s_inputAgain = true; }
    else        data->state = LV_INDEV_STATE_RELEASED;
}

/* Drain one queued remote sample per read; hold its level between events (a
 * pointer is level-triggered, so a drag's moves keep the press asserted until a
 * release event arrives). Ask LVGL to read again immediately while more are
 * queued so a fast tap's press+release both land. Runs on the lcd task. */
static void mirrorReadCb(lv_indev_t*, lv_indev_data_t* data) {
    MirrorPt p;
    if (s_mirrorQ && xQueueReceive(s_mirrorQ, &p, 0) == pdTRUE) s_mirrorLast = p;
    data->point.x = s_mirrorLast.x;
    data->point.y = s_mirrorLast.y;
    data->state   = s_mirrorLast.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (s_mirrorQ && uxQueueMessagesWaiting(s_mirrorQ) > 0) data->continue_reading = true;
}

/* Remote keys: pop one {key,pressed} per read, holding the last between events,
 * and continue while more are queued so a keystroke's press+release both land. */
static void mirrorKeyReadCb(lv_indev_t*, lv_indev_data_t* data) {
    static MirrorKey cur = { 0, 0 };
    MirrorKey k;
    if (s_mirrorKeyQ && xQueueReceive(s_mirrorKeyQ, &k, 0) == pdTRUE) cur = k;
    data->key   = cur.key;
    data->state = cur.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (s_mirrorKeyQ && uxQueueMessagesWaiting(s_mirrorKeyQ) > 0) data->continue_reading = true;
}

void lcdMirrorAttach(lcd_mirror_sink_t sink) { s_sink = sink; }

/* Remote input reaches the lcd task through lcdRun, which rides the lcd task's
 * ITS aux inbox — the SAME shallow inbox storage delivers CHANGED notifications
 * on. So mirror input must not spam it: a burst of pointer/key events posts at
 * most ONE pending lcdRun (s_mirrorReadPending), and that one drains BOTH queues
 * fully (a manual lv_indev_read reads one item and ignores continue_reading, so
 * loop per item — else keystroke press/release alternate and half the keys drop).
 * Flooding the inbox here backs up storage's subscriber delivery and can wedge
 * the whole system. No lcdNotifyActivity: the viewer session already holds the
 * panel awake (lcdMirrorKeepAwake), so per-event activity pokes are redundant. */
static volatile bool s_mirrorReadPending = false;

static void mirrorDrain(void*) {
    s_mirrorReadPending = false;
    if (s_mirrorIndev)
        while (s_mirrorQ && uxQueueMessagesWaiting(s_mirrorQ) > 0) lv_indev_read(s_mirrorIndev);
    if (s_mirrorKeyIndev)
        while (s_mirrorKeyQ && uxQueueMessagesWaiting(s_mirrorKeyQ) > 0) lv_indev_read(s_mirrorKeyIndev);
}

static void mirrorSchedule(void) {
    if (s_mirrorReadPending) return;
    s_mirrorReadPending = true;
    lcdRun(mirrorDrain);
}

void lcdMirrorInjectKey(uint32_t key) {
    if (!s_mirrorKeyQ) return;
    MirrorKey press = { key, 1 }, release = { key, 0 };
    xQueueSend(s_mirrorKeyQ, &press, 0);
    xQueueSend(s_mirrorKeyQ, &release, 0);
    mirrorSchedule();
}

void lcdMirrorInjectPointer(int16_t x, int16_t y, lcd_ptr_state_t state) {
    if (!s_mirrorQ) return;
    MirrorPt p = { x, y, (uint8_t)(state == LCD_PTR_PRESSED) };
    xQueueSend(s_mirrorQ, &p, 0);
    mirrorSchedule();   /* coalesced read on the lcd task (see mirrorSchedule) */
}

void lcdPointerSetVisibleMs(int ms) {
    s_ptrVisMs = ms;
    if (!s_cursor) return;
    if (ms < 0) {                                          /* always visible */
        if (s_ptrHide) { lv_timer_delete(s_ptrHide); s_ptrHide = nullptr; }
        lv_obj_remove_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    } else if (!lv_obj_has_flag(s_cursor, LV_OBJ_FLAG_HIDDEN)) {
        cursorPoke();                                      /* re-time a currently-shown cursor */
    }
}

/* ---- inactivity timeout -> standby, backlight fade, boot reveal ----
 * After s.lcd.inactivity_timeout seconds with no user input the lcd component does
 * NOT itself blank — it only sets the ephemeral `sys.standby` key. The board owns
 * what standby means: it subscribes to sys.standby and calls lcdScreenSleep() /
 * lcdScreenWake() (and powers its own input down/up). The board's centre button
 * sets/clears the same key, so the timeout and the button share one path. While
 * asleep the lcd loop stops rendering so the chip can light-sleep.
 *
 * The backlight is faded (not snapped) on wake and on the one-shot boot reveal, and
 * is held dark from boot until the launcher has settled with its icons placed — so
 * the UI never flashes on half-built. lcdScreenSleep snaps it to 0 (dark fast). */
static lv_timer_t* s_blankTimer = nullptr;
static int         s_blankMs    = 0;       /* <=0 = never time out */
static bool        s_mirrorHold = false;   /* a remote viewer is connected: stay awake, no blank */
static bool        s_screenOff  = false;   /* true == fully asleep: panel off, loop skips render */
static void        tickTimerRun(bool on);  /* LVGL tick esp_timer; stopped while blanked (defined below) */
static bool        s_fadingOut  = false;   /* backlight ramping down; still rendering to drive it */

/* Backlight: s_blTarget is the configured on-level (s.lcd.backlight); s_blCur is
 * the live duty an lv_anim eases toward it (starts dark — backlightInit duty 0). */
static uint8_t     s_blTarget   = 200;
static int32_t     s_blCur      = 0;
static bool        s_booted     = false;   /* boot reveal done (backlight allowed up) */
static lv_timer_t* s_settle     = nullptr; /* debounce: reveal once icon loads quiesce */
static lv_timer_t* s_revealCap  = nullptr; /* hard cap so boot always reveals */

static void blAnimExec(void* var, int32_t v) { (void)var; lcdPanelBacklight((uint8_t)v); s_blCur = v; }

static void backlightFadeTo(int32_t level, uint32_t ms, lv_anim_completed_cb_t done = nullptr) {
    lv_anim_delete(&s_blCur, blAnimExec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_blCur);
    lv_anim_set_exec_cb(&a, blAnimExec);
    lv_anim_set_values(&a, s_blCur, level);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

void lcdBacklightSetTarget(uint8_t level) {
    s_blTarget = level;
    /* Live slider change: apply at once while awake and past the boot reveal;
     * before the reveal (or while asleep) just remember it — the fade-in uses it. */
    if (s_booted && !s_screenOff) {
        lv_anim_delete(&s_blCur, blAnimExec);
        lcdPanelBacklight(level);
        s_blCur = level;
    }
}

static void bootReveal(void) {
    if (s_booted) return;
    s_booted = true;
    if (s_settle)    { lv_timer_delete(s_settle);    s_settle = nullptr; }
    if (s_revealCap) { lv_timer_delete(s_revealCap); s_revealCap = nullptr; }
    if (!s_screenOff) backlightFadeTo(s_blTarget, 300);
}

/* Each launcher icon that lands pushes the reveal out a little; when the loads go
 * quiet the screen lights with everything already placed. lcdLvglInit arms a hard
 * cap so a board with no icons (or a stuck loader) still reveals. */
void lcdBootSettleKick(void) {
    if (s_booted) return;
    if (s_settle) { lv_timer_reset(s_settle); return; }
    s_settle = lv_timer_create([](lv_timer_t*) { s_settle = nullptr; bootReveal(); }, 200, nullptr);
    lv_timer_set_repeat_count(s_settle, 1);
}

static void armBlankTimer(void) {
    if (s_blankTimer) { lv_timer_delete(s_blankTimer); s_blankTimer = nullptr; }
    /* A connected remote viewer holds the panel awake: never arm the blank timer
     * until it disconnects (lcdMirrorKeepAwake). */
    if (s_mirrorHold || s_blankMs <= 0 || s_screenOff) return;
    s_blankTimer = lv_timer_create([](lv_timer_t*) { s_blankTimer = nullptr; storageSet("sys.standby", 1); },
                                   (uint32_t)s_blankMs, nullptr);
    lv_timer_set_repeat_count(s_blankTimer, 1);   /* one-shot: fires once, self-deletes */
}

/* End of the sleep fade-out: now that the backlight is at 0, actually power the
 * panel off and let the loop stop rendering. Powering off mid-fade would cut to
 * black instead of ramping, so it has to wait for the anim to finish. */
static void blSleepDone(lv_anim_t*) {
    if (!s_fadingOut) return;   /* a wake cancelled the fade-out before it landed */
    s_fadingOut = false;
    s_screenOff = true;
    lcdPanelDisplayPower(false);   /* panel standby, GRAM retained */
    tickTimerRun(false);           /* no rendering while blanked → let the chip light-sleep */
}

void lcdScreenSleep(void) {
    if (s_screenOff || s_fadingOut) return;
    if (s_blankTimer) { lv_timer_delete(s_blankTimer); s_blankTimer = nullptr; }
    /* Ramp the backlight down (the loop keeps rendering to drive the anim while
     * s_screenOff is still false); blSleepDone powers the panel off at 0. */
    s_fadingOut = true;
    backlightFadeTo(0, 300, blSleepDone);
}

void lcdScreenWake(void) {
    if (!s_screenOff && !s_fadingOut) return;   /* already awake / fading in */
    bool wasOff = s_screenOff;
    s_fadingOut = false;                          /* cancel a fade-out in progress */
    s_screenOff = false;
    if (wasOff) {
        tickTimerRun(true);                       /* resume LVGL time before the fade anim runs */
        lcdPanelDisplayPower(true);               /* panel was off — bring it back */
    }
    backlightFadeTo(s_blTarget, 300);             /* fade up from the current duty */
    armBlankTimer();
}

void lcdInactivitySetTimeout(int seconds) {
    s_blankMs = seconds > 0 ? seconds * 1000 : 0;
    armBlankTimer();
}

void lcdMirrorKeepAwake(bool on) {
    lcdRun([](void* a) {
        s_mirrorHold = (bool)(intptr_t)a;
        if (s_mirrorHold) {
            lcdScreenWake();                /* pull out of standby for the viewer */
            if (s_blankTimer) { lv_timer_delete(s_blankTimer); s_blankTimer = nullptr; }
        } else {
            armBlankTimer();                /* restore the configured timeout */
        }
    }, (void*)(intptr_t)on);
}

/* Register user input: just re-arm the inactivity timer. Waking from standby is the
 * board's job (it clears sys.standby), not ours, so this no longer wakes and always
 * returns false — kept bool for lcdNotifyActivity's callers. */
bool lcdActivity(void) {
    if (s_blankTimer) lv_timer_reset(s_blankTimer);
    else              armBlankTimer();   /* (re)arm if a setting change left it off */
    return false;
}

bool lcdScreenIsOff(void) { return s_screenOff; }

/* The hardware keyboard, if any, is a CONSUMER concern (its quirks vary per
 * board): the consumer creates its own keypad indev, joins it to lcdInputGroup(),
 * and drives it via lcdRun(). The lcd component knows nothing about it. See
 * reticulous/main/tdeck.cpp for the T-Deck implementation. */

static void tickCb(void*) { lv_tick_inc(LCD_TICK_MS); }

/* The LVGL tick is a 2 ms periodic esp_timer — a SYSTIMER-backed wake that fires
 * ~500×/s. LVGL only needs it while we're actually rendering; left running while
 * the screen is blanked it wakes the chip every 2 ms and defeats automatic light
 * sleep (the dominant idle drain on battery). So stop it when the panel powers
 * off and restart it on wake, before any fade animation needs to advance. */
static esp_timer_handle_t s_tickTimer = nullptr;

static void tickTimerRun(bool on) {
    if (!s_tickTimer) return;
    if (on) {
        /* start_periodic on an already-running timer errors; stop first (no-op if
         * idle). lv_tick freezes across sleep and simply resumes — LVGL time is
         * relative, so any timer that came due mid-sleep just fires once on wake. */
        esp_timer_stop(s_tickTimer);
        esp_timer_start_periodic(s_tickTimer, LCD_TICK_MS * 1000);
    } else {
        esp_timer_stop(s_tickTimer);
    }
}

bool lcdLvglInit(void) {
    esp_lcd_panel_io_handle_t io = nullptr;
    s_panel = lcdPanelInit(&io, &s_w, &s_h);
    if (!s_panel || !io || s_w <= 0 || s_h <= 0) {
        err("panel init failed\n");
        return false;
    }

    lv_init();

    /* Route LVGL's internal diagnostics (freetype bring-up, asserts, memory
     * warnings) into our logger under the 'lvgl' tag instead of printf, so they
     * land in the device log with everything else and honour per-tag levels. */
#if LV_USE_LOG
    lv_log_register_print_cb([](lv_log_level_t level, const char* buf) {
        /* buf carries LVGL's own "[Warn] file:line\t" prefix + newline; pass it
         * through, mapped to our severity. */
        switch (level) {
            case LV_LOG_LEVEL_ERROR: ESP_LOGE("lvgl", "%s", buf); break;
            case LV_LOG_LEVEL_WARN:  ESP_LOGW("lvgl", "%s", buf); break;
            case LV_LOG_LEVEL_INFO:  ESP_LOGI("lvgl", "%s", buf); break;
            default:                 ESP_LOGD("lvgl", "%s", buf); break;
        }
    });
#endif

    s_disp = lv_display_create(s_w, s_h);
    if (!s_disp) return false;
    lv_display_set_user_data(s_disp, s_panel);
    lv_display_set_flush_cb(s_disp, flushCb);

    int lines = LCD_DRAW_BUDGET / (s_w * (int)sizeof(lv_color16_t));
    if (lines < 1) lines = 1;
    size_t bufSz = (size_t)s_w * lines * sizeof(lv_color16_t);
    void* buf1 = heap_caps_malloc(bufSz, MALLOC_CAP_DMA);
    void* buf2 = heap_caps_malloc(bufSz, MALLOC_CAP_DMA);
    if (!buf1 || !buf2) { err("draw-buffer alloc failed (%u B)\n", (unsigned)bufSz); return false; }
    lv_display_set_buffers(s_disp, buf1, buf2, bufSz, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Per-strip DMA-done signal (consumed synchronously by flushCb). */
    s_dmaDone = xSemaphoreCreateBinary();
    if (!s_dmaDone) { err("dmaDone sem alloc failed\n"); return false; }
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = onColorDone };
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, s_disp);

    /* LVGL tick from a periodic esp_timer. */
    esp_timer_create_args_t targs = {};
    targs.callback = tickCb;
    targs.name     = "lvgl_tick";
    esp_timer_create(&targs, &s_tickTimer);
    esp_timer_start_periodic(s_tickTimer, LCD_TICK_MS * 1000);

    /* Focus group for non-pointer input (populated by the launcher). */
    s_group = lv_group_create();

    /* Board input setup (create touch handles, wire input INT lines) — on the lcd
     * task, with the panel and the shared GPIO ISR service already up. */
    const lcd_input_t* in = lcdInput();
    if (in && in->init) in->init();

    /* All indevs are EVENT-mode: LVGL never runs a read timer for them, so there
     * is no input polling. The board's INT lines (via lcdInputISR) wake the lcd
     * task, which calls lcdInputPoll() → lv_indev_read() only on a real edge. */

    /* Optional touch — pointer indev. Absent boards run without it. */
    if (in && in->touch_read) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touchReadCb);
        lv_indev_set_display(s_indev, s_disp);
        lv_indev_set_mode(s_indev, LV_INDEV_MODE_EVENT);
        lv_indev_set_scroll_throw(s_indev, 100);   /* no scroll momentum (see below) */
    } else {
        info("no touch panel — pointer indev disabled\n");
    }

    /* Optional cursor device (T-Deck trackball) — a second pointer indev with a
     * visible, auto-hiding cursor. The board owns the position (pointer_read); we
     * own the cursor and route the centre button to it as the click, so the
     * keypad button indev below is skipped when a cursor device is present. */
    if (in && in->pointer_read) {
        s_cursor = lv_obj_create(lv_layer_sys());      /* above the status bar */
        lv_obj_remove_style_all(s_cursor);
        lv_obj_remove_flag(s_cursor, LV_OBJ_FLAG_CLICKABLE);   /* never a hit target */
        lv_obj_set_size(s_cursor, LCD_CURSOR_SIZE, LCD_CURSOR_SIZE);
        lv_obj_set_style_radius(s_cursor, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_cursor, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_cursor, LV_OPA_50, 0);
        lv_obj_set_style_border_color(s_cursor, lv_color_black(), 0);
        lv_obj_set_style_border_width(s_cursor, 2, 0);
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN); /* shown on motion / poke */

        s_ptrIndev = lv_indev_create();
        lv_indev_set_type(s_ptrIndev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_ptrIndev, pointerReadCb);
        lv_indev_set_display(s_ptrIndev, s_disp);
        /* Not lv_indev_set_cursor(): that snaps the cursor to the point on every
         * read (the zap). We position s_cursor ourselves in pointerReadCb via
         * cursorGlideTo() so it eases to each new spot. */
        lv_indev_set_mode(s_ptrIndev, LV_INDEV_MODE_EVENT);
        lv_indev_set_scroll_throw(s_ptrIndev, 100);   /* no scroll momentum (see below) */
        lcdPointerSetVisibleMs(s_ptrVisMs);   /* apply dwell now the cursor exists
                                                 (the owner may have set it earlier) */
    }

    /* Screen-mirror remote pointer indev — always present (any board), fed by
     * lcdMirrorInjectPointer. EVENT mode: read only when an injected sample forces
     * it, so it costs nothing when no viewer is connected. */
    s_mirrorQ = xQueueCreate(32, sizeof(MirrorPt));
    s_mirrorIndev = lv_indev_create();
    lv_indev_set_type(s_mirrorIndev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_mirrorIndev, mirrorReadCb);
    lv_indev_set_display(s_mirrorIndev, s_disp);
    lv_indev_set_mode(s_mirrorIndev, LV_INDEV_MODE_EVENT);
    lv_indev_set_scroll_throw(s_mirrorIndev, 100);   /* no scroll momentum (see below) */

    /* Screen-mirror remote keyboard — a keypad indev on the focus group, fed by
     * lcdMirrorInjectKey, so a browser viewer can type into the CLI / text fields
     * exactly like the board's own keyboard does. */
    s_mirrorKeyQ = xQueueCreate(64, sizeof(MirrorKey));
    s_mirrorKeyIndev = lv_indev_create();
    lv_indev_set_type(s_mirrorKeyIndev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_mirrorKeyIndev, mirrorKeyReadCb);
    lv_indev_set_display(s_mirrorKeyIndev, s_disp);
    lv_indev_set_group(s_mirrorKeyIndev, s_group);
    lv_indev_set_mode(s_mirrorKeyIndev, LV_INDEV_MODE_EVENT);

    /* Scroll momentum is disabled on every pointer indev (scroll_throw=100 kills
     * the throw vector immediately, so a flick stops without a glide). The
     * post-release deceleration animates a run of tiny residual scroll steps —
     * cheap on the physical SPI panel but a real cost to the browser screen-mirror,
     * which pays a compressed frame per step for motion no one is driving. Dropping
     * it makes scrolling crisp and keeps the mirror's frame budget for real moves. */

    /* Optional hardware button (T-Deck centre/Home), only when no cursor device
     * claimed it. Keypad indev on the focus group; the board's click_read() drives
     * the click and owns the hold→home policy. */
    if (in && in->click_read && !in->pointer_read) {
        s_btnIndev = lv_indev_create();
        lv_indev_set_type(s_btnIndev, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_read_cb(s_btnIndev, buttonReadCb);
        lv_indev_set_display(s_btnIndev, s_disp);
        lv_indev_set_group(s_btnIndev, s_group);
        lv_indev_set_mode(s_btnIndev, LV_INDEV_MODE_EVENT);
    }

    /* A hardware keyboard, if present, is set up by the consumer (its own keypad
     * indev joined to s_group via lcdInputGroup()) — see lcdSetHasKeyboard() and
     * reticulous/main/tdeck.cpp. The lcd component creates no keyboard indev. */

    /* Boot reveal hard cap: the backlight stays dark until the launcher settles
     * (lcdBootSettleKick as icons land), but light it regardless after this so a
     * board with no icons or a stuck loader never boots to a black screen. */
    s_revealCap = lv_timer_create([](lv_timer_t*) { s_revealCap = nullptr; bootReveal(); }, 3000, nullptr);
    lv_timer_set_repeat_count(s_revealCap, 1);

    return true;
}

bool lcdInputPoll(void) {
    s_inputAgain = false;
    if (s_indev)    lv_indev_read(s_indev);
    if (s_ptrIndev) lv_indev_read(s_ptrIndev);
    if (s_btnIndev) lv_indev_read(s_btnIndev);
    return s_inputAgain;
}

/* Off-task touch drive (lcd_input.h). Like the mirror path, coalesce to one
 * pending lcdRun so a board sampling touch every few ms can't flood the lcd
 * task's ITS aux inbox (which also carries storage notifications). The hop reads
 * only the touch indev; ongoing tracking is then sustained by touchReadCb's own
 * 10ms re-read timer, so the board bumps once per gesture, not once per sample. */
static volatile bool s_touchPollPending = false;

static void touchPollDrain(void*) {
    s_touchPollPending = false;
    if (s_indev) lv_indev_read(s_indev);
}

void lcdTouchPoll(void) {
    if (!s_indev || s_touchPollPending) return;
    s_touchPollPending = true;
    lcdRun(touchPollDrain);
}

/* Pause the per-indev read timer LVGL keeps for its own press timing whenever the
 * indev is released. LVGL resumes it on press (lv_indev.c) and is supposed to
 * pause it on release; when that's missed the timer keeps auto-reading the indev
 * at ~30 Hz, which for a pointer also repositions the cursor → redraws → idle CPU
 * for no reason. spangap reads every indev manually (event mode) and handles its
 * own press/hold timing, so these timers should never run when nothing is held. */
void lcdPauseIdleInputTimers(void) {
    lv_indev_t* devs[] = { s_indev, s_ptrIndev, s_btnIndev, s_mirrorIndev, s_mirrorKeyIndev };
    for (lv_indev_t* in : devs) {
        if (!in || lv_indev_get_state(in) != LV_INDEV_STATE_RELEASED) continue;
        lv_timer_t* rt = lv_indev_get_read_timer(in);
        if (rt && !lv_timer_get_paused(rt)) {
            lv_timer_pause(rt);
            dbg("paused idle indev read-timer\n");   /* TEMP: confirm the quiescent-CPU fix */
        }
    }
}

int         lcdScreenW(void)     { return s_w; }
int         lcdScreenH(void)     { return s_h; }
void        lcdDisplaySize(int* w, int* h) { if (w) *w = s_w; if (h) *h = s_h; }
lv_group_t* lcdInputGroup(void)  { return s_group; }

/* ---- trackball → arrow keys (see lcdProgramScrollwheelArrows) ---- */
static bool s_swArrows = false;
void lcdScrollwheelArrowsApply(bool on) {
    s_swArrows = on;
    if (on && s_cursor) lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);   /* no pointer in arrow mode */
}
bool lcdScrollwheelArrowsActive(void) { return s_swArrows; }
