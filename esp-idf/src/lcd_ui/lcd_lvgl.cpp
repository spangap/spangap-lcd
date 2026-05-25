/**
 * lcd_lvgl.cpp — LVGL v9 bring-up over esp_lcd.
 *
 * Brings up the panel via the board HAL, creates the LVGL display with a
 * partial double buffer, wires the flush + DMA-done + tick callbacks, and (if
 * the board has touch) a pointer indev. A focus group is created for future
 * encoder/keypad indevs so a trackball-only board can drive the same UI.
 */
#include "lcd_internal.h"
#include "lcd_board.h"

#include "log.h"
#include "spi_helper.h"
#include "storage.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"

/* Draw-strip byte budget. Each flush is one SPI transfer, so a strip must fit
 * the shared-bus max_transfer_sz — the SD driver brings the bus up at 4096
 * (fs.cpp) before us, and spiHelperInitBus is first-caller-wins. Strip line
 * count is derived from this and the panel width so it holds for any size.
 * Two strips are double-buffered in internal DMA-capable RAM. */
#define LCD_DRAW_BUDGET 4000
#define LCD_TICK_MS     2

static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_display_t*          s_disp  = nullptr;
static esp_lcd_touch_handle_t s_touch = nullptr;
static lv_indev_t*            s_indev = nullptr;
static lv_indev_t*            s_btnIndev = nullptr;   /* hardware Home/centre button */
static lv_group_t*            s_group = nullptr;
static int                    s_w = 0, s_h = 0;
static SemaphoreHandle_t      s_dmaDone = nullptr;   /* given when a strip's DMA completes */

/* Event-mode input bookkeeping (all touched only on the lcd task). The indevs
 * are LV_INDEV_MODE_EVENT, so they only read when lcdInputPoll() drives them. */
static lv_timer_t*            s_touchTrack = nullptr; /* 10ms re-read while a finger is down */
static lv_timer_t*            s_btnHold    = nullptr; /* 300ms "hold = go home" one-shot */
static bool                   s_btnLong    = false;   /* hold fired → suppress the release click */
static bool                   s_inputAgain = false;   /* a read cb wants an immediate follow-up read */
static bool                   s_touchSwallow = false; /* drop the touch that woke the screen until it lifts */

/* Multi-touch / gestures (off by default — single-touch is the cheapest path).
 * When on, touchReadCb reads all fingers and dispatches to gesture handlers;
 * >=2 fingers suppress the single pointer so the gesture owner has the gesture. */
#define LCD_TOUCH_MAXPTS 5
static volatile bool          s_multipoint = false;
static lcd_gesture_cb_t       s_gestureCb[4] = {};

/* Trackball/mouse pointer (board->pointer_read). The board owns the position; we
 * own the LVGL pointer indev, the cursor object, and its show-on-move/auto-hide. */
static lv_indev_t*            s_ptrIndev   = nullptr;
static lv_obj_t*              s_cursor     = nullptr;
static lv_timer_t*            s_ptrHide    = nullptr; /* one-shot: hide the cursor after inactivity */
static int                    s_ptrVisMs   = 2000;    /* <0 = always visible */

/* Strip DMA completed (ISR context). Wakes the flush, which then drops the
 * shared-bus lock. */
static bool onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
                        void*) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_dmaDone, &hp);
    return hp == pdTRUE;
}

static void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    auto panel = static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
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

static void touchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
    auto tp = static_cast<esp_lcd_touch_handle_t>(lv_indev_get_user_data(indev));
    esp_lcd_touch_point_data_t pt[LCD_TOUCH_MAXPTS] = {};
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(tp);
    esp_lcd_touch_get_data(tp, pt, &cnt, s_multipoint ? LCD_TOUCH_MAXPTS : 1);

    /* GT911 reports raw native-portrait coords (esp_lcd_touch left identity).
     * Rotate each to our landscape display: screen_x = raw_y, screen_y = (H-1) -
     * raw_x. Clamp to the panel so edge touches never wrap off-screen. */
    int n = cnt > LCD_TOUCH_MAXPTS ? LCD_TOUCH_MAXPTS : cnt;
    lcd_touch_pt_t gp[LCD_TOUCH_MAXPTS];
    for (int i = 0; i < n; i++) {
        int x = pt[i].y;
        int y = (s_h - 1) - pt[i].x;
        if (x < 0) x = 0; else if (x >= s_w) x = s_w - 1;
        if (y < 0) y = 0; else if (y >= s_h) y = s_h - 1;
        gp[i].x = (int16_t)x; gp[i].y = (int16_t)y;
    }

    /* Keep re-reading while any finger is down (the GT911 INT only guarantees the
     * first edge) — also how a swallowed wake-touch is polled to release. */
    if (n > 0) {
        if (!s_touchTrack) s_touchTrack = lv_timer_create(touchTrackCb, 10, nullptr);
    } else {
        s_touchSwallow = false;   /* finger lifted — the next touch is a real one */
        if (s_touchTrack) { lv_timer_delete(s_touchTrack); s_touchTrack = nullptr; }
    }

    /* Gesture dispatch (multipoint only). */
    if (s_multipoint)
        for (auto cb : s_gestureCb) if (cb) cb(gp, n);

    /* Single pointer for LVGL: point 0 drives it, but >=2 fingers (multipoint)
     * suppress it so the gesture owner has the interaction; a screen-waking touch
     * is swallowed until lift (the GT911 re-fires its INT every frame the finger
     * stays down, which would otherwise land as a real press). */
    static bool wasDown = false;
    bool suppress = (s_multipoint && n >= 2);
    if (n > 0 && !suppress && !s_touchSwallow) {
        data->point.x = gp[0].x;
        data->point.y = gp[0].y;
        data->state   = LV_INDEV_STATE_PRESSED;
        if (!wasDown) dbg("touch -> (%d,%d)\n", gp[0].x, gp[0].y);
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

/* Hardware Home/centre button as a keypad indev (board->button_read). It's also
 * the trackball's click, so we mustn't eat every press: a short press emits an
 * ENTER click on the focused item (synthesized as a press+release on two reads),
 * while a hold of >=300ms returns to the launcher and is NOT clicked.
 *
 * Event-driven: the button INT wakes the press and release edges; the 300ms hold is
 * a one-shot lv_timer (s_btnHold) armed on press — so there is no per-frame
 * polling during a hold, only a single deadline. All on the lcd task. */
static void btnHoldCb(lv_timer_t*) {
    s_btnHold = nullptr;            /* the one-shot deleted itself after this fire */
    s_btnLong = true;              /* tell the release edge not to make it a click */
    lcdGoHomeInternal();
}

/* The centre/Home button's click-vs-hold state machine, shared by the keypad
 * button indev and the trackball pointer indev (whichever owns GPIO 0). It never
 * reports the raw hold as a press: a short press is replayed as a synthesized
 * press+release over two reads (driven by s_inputAgain), and a >=300 ms hold fires
 * Home via a one-shot lv_timer and is NOT clicked. Returns the press/release to
 * emit this read, or BTN_NONE (caller reports released). */
enum btn_action_t { BTN_NONE, BTN_PRESS, BTN_RELEASE };
static btn_action_t centerButtonStep(bool down) {
    static enum { IDLE, HELD, CLICK_PRESS, CLICK_RELEASE } phase = IDLE;

    if (phase == CLICK_PRESS)   { phase = CLICK_RELEASE; s_inputAgain = true; return BTN_PRESS; }
    if (phase == CLICK_RELEASE) { phase = IDLE;                                return BTN_RELEASE; }

    if (down) {
        if (phase == IDLE) {
            phase = HELD;
            s_btnLong = false;
            s_btnHold = lv_timer_create(btnHoldCb, 300, nullptr);   /* 300ms hold = go home */
            lv_timer_set_repeat_count(s_btnHold, 1);   /* one-shot: fires once, self-deletes */
        }
        return BTN_NONE;                          /* never report the raw hold */
    }

    if (s_btnHold) { lv_timer_delete(s_btnHold); s_btnHold = nullptr; }
    if (phase == HELD && !s_btnLong) { phase = CLICK_PRESS; s_inputAgain = true; }  /* short press -> click */
    else                              phase = IDLE;                                 /* hold already fired */
    return BTN_NONE;
}

static void buttonReadCb(lv_indev_t*, lv_indev_data_t* data) {
    const lcd_board_t* board = lcdBoard();
    bool down = board && board->button_read && board->button_read();
    data->key   = LV_KEY_ENTER;
    data->state = (centerButtonStep(down) == BTN_PRESS) ? LV_INDEV_STATE_PRESSED
                                                        : LV_INDEV_STATE_RELEASED;
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

static void pointerReadCb(lv_indev_t*, lv_indev_data_t* data) {
    const lcd_board_t* board = lcdBoard();
    int x = 0, y = 0;
    if (board && board->pointer_read && board->pointer_read(&x, &y)) cursorPoke();  /* moved */
    data->point.x = x;
    data->point.y = y;

    bool down = board && board->button_read && board->button_read();
    if (centerButtonStep(down) == BTN_PRESS) { cursorPoke(); data->state = LV_INDEV_STATE_PRESSED; }
    else                                       data->state = LV_INDEV_STATE_RELEASED;
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

/* ---- inactivity blank / screen standby ----
 * After s.lcd.inactivity_timeout seconds with no user input the screen blanks:
 * backlight off + (if the board supports it) panel standby, GRAM retained. Any
 * input wakes it (lcdActivity); the lcd loop skips rendering while off so the
 * chip can light-sleep. Same one-shot lv_timer pattern as the cursor auto-hide. */
static lv_timer_t* s_blankTimer = nullptr;
static int         s_blankMs    = 0;       /* <=0 = never blank */
static bool        s_screenOff  = false;

static void screenSleep(void);

static void armBlankTimer(void) {
    if (s_blankTimer) { lv_timer_delete(s_blankTimer); s_blankTimer = nullptr; }
    if (s_blankMs <= 0 || s_screenOff) return;
    s_blankTimer = lv_timer_create([](lv_timer_t*) { s_blankTimer = nullptr; screenSleep(); },
                                   (uint32_t)s_blankMs, nullptr);
    lv_timer_set_repeat_count(s_blankTimer, 1);   /* one-shot: fires once, self-deletes */
}

static void screenSleep(void) {
    if (s_screenOff) return;
    s_screenOff = true;
    if (s_blankTimer) { lv_timer_delete(s_blankTimer); s_blankTimer = nullptr; }
    const lcd_board_t* b = lcdBoard();
    if (b && b->backlight)     b->backlight(0);
    if (b && b->display_power) b->display_power(false);   /* panel standby */
}

static void screenWake(void) {
    if (!s_screenOff) return;
    s_screenOff = false;
    const lcd_board_t* b = lcdBoard();
    if (b && b->display_power) b->display_power(true);
    if (b && b->backlight)     b->backlight((uint8_t)storageGetInt("s.lcd.backlight", 200));
    armBlankTimer();
}

void lcdInactivitySetTimeout(int seconds) {
    s_blankMs = seconds > 0 ? seconds * 1000 : 0;
    armBlankTimer();
}

bool lcdActivity(void) {
    if (s_screenOff) { screenWake(); return true; }
    if (s_blankTimer) lv_timer_reset(s_blankTimer);
    else              armBlankTimer();   /* (re)arm if a setting change left it off */
    return false;
}

bool lcdScreenIsOff(void) { return s_screenOff; }

/* Called from the lcd loop when an input edge woke the screen. The button/trackball
 * wake edge is already dropped (the loop skips that poll); touch needs more, since
 * the GT911 re-fires its INT every frame the finger stays down. Arm a swallow so
 * touchReadCb drops the rest of the waking press; it self-clears when the finger
 * lifts. No-op for a button/trackball wake (no finger down → cleared on the next
 * touch read). */
void lcdSwallowTouch(void) { s_touchSwallow = true; }

/* The hardware keyboard, if any, is a CONSUMER concern (its quirks vary per
 * board): the consumer creates its own keypad indev, joins it to lcdInputGroup(),
 * and drives it via lcdRun(). The lcd component knows nothing about it. See
 * reticulous/main/tdeck.cpp for the T-Deck implementation. */

static void tickCb(void*) { lv_tick_inc(LCD_TICK_MS); }

bool lcdLvglInit(void) {
    const lcd_board_t* board = lcdBoard();
    if (!board || !board->init) {
        err("no board registered — call lcdSetBoard() before diptychInit()\n");
        return false;
    }
    esp_lcd_panel_io_handle_t io = nullptr;
    s_panel = board->init(&io, &s_w, &s_h);
    if (!s_panel || !io || s_w <= 0 || s_h <= 0) {
        err("board panel init failed\n");
        return false;
    }

    lv_init();

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
    esp_timer_handle_t th = nullptr;
    esp_timer_create(&targs, &th);
    esp_timer_start_periodic(th, LCD_TICK_MS * 1000);

    /* Focus group for non-pointer input (populated by the launcher). */
    s_group = lv_group_create();

    /* All indevs are EVENT-mode: LVGL never runs a read timer for them, so there
     * is no input polling. The board's INT lines (via lcdInputISR) wake the lcd
     * task, which calls lcdInputPoll() → lv_indev_read() only on a real edge. */

    /* Optional touch — pointer indev. Absent boards run without it. */
    s_touch = board->touch_init ? board->touch_init() : nullptr;
    if (s_touch) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touchReadCb);
        lv_indev_set_user_data(s_indev, s_touch);
        lv_indev_set_display(s_indev, s_disp);
        lv_indev_set_mode(s_indev, LV_INDEV_MODE_EVENT);
    } else {
        info("no touch panel — pointer indev disabled\n");
    }

    /* Optional cursor device (T-Deck trackball) — a second pointer indev with a
     * visible, auto-hiding cursor. The board owns the position (pointer_read); we
     * own the cursor and route the centre button to it as the click, so the
     * keypad button indev below is skipped when a cursor device is present. */
    if (board->pointer_read) {
        s_cursor = lv_obj_create(lv_layer_sys());      /* above the status bar */
        lv_obj_remove_style_all(s_cursor);
        lv_obj_remove_flag(s_cursor, LV_OBJ_FLAG_CLICKABLE);   /* never a hit target */
        lv_obj_set_size(s_cursor, 14, 14);
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
        lv_indev_set_cursor(s_ptrIndev, s_cursor);
        lv_indev_set_mode(s_ptrIndev, LV_INDEV_MODE_EVENT);
        lcdPointerSetVisibleMs(s_ptrVisMs);   /* apply dwell now the cursor exists
                                                 (the owner may have set it earlier) */
    }

    /* Optional hardware button (T-Deck centre/Home), only when no cursor device
     * claimed it. Keypad indev on the focus group: short press clicks the focused
     * tile, >=300ms hold returns to the launcher. */
    if (board->button_read && !board->pointer_read) {
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

    return true;
}

bool lcdInputPoll(void) {
    s_inputAgain = false;
    if (s_indev)    lv_indev_read(s_indev);
    if (s_ptrIndev) lv_indev_read(s_ptrIndev);
    if (s_btnIndev) lv_indev_read(s_btnIndev);
    return s_inputAgain;
}

/* Pause the per-indev read timer LVGL keeps for its own press timing whenever the
 * indev is released. LVGL resumes it on press (lv_indev.c) and is supposed to
 * pause it on release; when that's missed the timer keeps auto-reading the indev
 * at ~30 Hz, which for a pointer also repositions the cursor → redraws → idle CPU
 * for no reason. diptych reads every indev manually (event mode) and handles its
 * own press/hold timing, so these timers should never run when nothing is held. */
void lcdPauseIdleInputTimers(void) {
    lv_indev_t* devs[] = { s_indev, s_ptrIndev, s_btnIndev };
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
lv_group_t* lcdInputGroup(void)  { return s_group; }
