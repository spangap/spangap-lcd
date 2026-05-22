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
static lv_indev_t*            s_kbIndev = nullptr;    /* hardware QWERTY keyboard */
static lv_group_t*            s_group = nullptr;
static int                    s_w = 0, s_h = 0;
static SemaphoreHandle_t      s_dmaDone = nullptr;   /* given when a strip's DMA completes */

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

static void touchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
    auto tp = static_cast<esp_lcd_touch_handle_t>(lv_indev_get_user_data(indev));
    esp_lcd_touch_point_data_t pt[1] = {};
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(tp);
    esp_lcd_touch_get_data(tp, pt, &cnt, 1);
    /* Poll fast (10ms) while a finger is down for smooth tracking/gestures;
     * relax to ~30ms when up. */
    lv_timer_set_period(lv_indev_get_read_timer(indev), cnt > 0 ? 10 : 30);
    static bool wasDown = false;   /* TEMP calib: log on the press edge only */
    if (cnt > 0) {
        /* GT911 reports raw native-portrait coords (esp_lcd_touch left identity).
         * Rotate to our landscape display: screen_x = raw_y, screen_y = (H-1) -
         * raw_x. Clamp to the panel so edge touches never wrap off-screen. */
        int x = pt[0].y;
        int y = (s_h - 1) - pt[0].x;
        if (x < 0) x = 0; else if (x >= s_w) x = s_w - 1;
        if (y < 0) y = 0; else if (y >= s_h) y = s_h - 1;
        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PRESSED;
        if (!wasDown) info("touch raw=(%d,%d) -> (%d,%d)\n", pt[0].x, pt[0].y, x, y);
        wasDown = true;
    } else {
        data->state   = LV_INDEV_STATE_RELEASED;
        wasDown = false;
    }
}

/* Hardware Home/centre button as a keypad indev (board->button_read). It's also
 * the trackball's click, so we mustn't eat every press: a short press emits an
 * ENTER click on the focused item (synthesized as a press+release on two reads),
 * while a hold of >=1s returns to the launcher and is NOT clicked. All on the
 * lcd task — LVGL polls this from lv_timer_handler. */
static void buttonReadCb(lv_indev_t*, lv_indev_data_t* data) {
    const lcd_board_t* board = lcdBoard();
    bool down = board && board->button_read && board->button_read();

    static enum { IDLE, HELD, LONG_FIRED, CLICK_PRESS, CLICK_RELEASE } phase = IDLE;
    static uint32_t pressedAt = 0;

    data->key = LV_KEY_ENTER;

    /* Replay a synthesized short click across two reads. */
    if (phase == CLICK_PRESS)   { data->state = LV_INDEV_STATE_PRESSED;  phase = CLICK_RELEASE; return; }
    if (phase == CLICK_RELEASE) { data->state = LV_INDEV_STATE_RELEASED; phase = IDLE;          return; }

    if (down) {
        if (phase == IDLE) { phase = HELD; pressedAt = lv_tick_get(); }
        if (phase == HELD && lv_tick_elaps(pressedAt) >= 1000) {
            lcdGoHomeInternal();                 /* hold >=1s -> launcher */
            phase = LONG_FIRED;
        }
        data->state = LV_INDEV_STATE_RELEASED;   /* never report the raw hold as a click */
        return;
    }

    if (phase == HELD) phase = CLICK_PRESS;      /* released before 1s -> it was a click */
    else               phase = IDLE;             /* released after a long press -> reset */
    data->state = LV_INDEV_STATE_RELEASED;
}

/* Hardware QWERTY keyboard as a keypad indev. board->key_read returns the next
 * raw ASCII char (0 = none); we map a few control codes to LVGL keys and pass
 * printable ASCII through. Each char is emitted as a press+release over two
 * reads so the focused textarea registers exactly one keystroke. */
static uint32_t mapAsciiKey(uint32_t b) {
    switch (b) {
        case 13: case 10:  return LV_KEY_ENTER;
        case 8:  case 127: return LV_KEY_BACKSPACE;
        case 27:           return LV_KEY_ESC;
        default:           return (b >= 0x20 && b < 0x7F) ? b : 0;
    }
}

static void keyboardReadCb(lv_indev_t*, lv_indev_data_t* data) {
    static uint32_t held = 0;
    if (held) { data->key = held; data->state = LV_INDEV_STATE_RELEASED; held = 0; return; }
    const lcd_board_t* board = lcdBoard();
    uint32_t k = (board && board->key_read) ? mapAsciiKey(board->key_read()) : 0;
    if (k) { data->key = k; data->state = LV_INDEV_STATE_PRESSED; held = k; }
    else   { data->state = LV_INDEV_STATE_RELEASED; }
}

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

    /* Optional touch — pointer indev. Absent boards run without it. */
    s_touch = board->touch_init ? board->touch_init() : nullptr;
    if (s_touch) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touchReadCb);
        lv_indev_set_user_data(s_indev, s_touch);
        lv_indev_set_display(s_indev, s_disp);
    } else {
        info("no touch panel — pointer indev disabled\n");
    }

    /* Optional hardware button (T-Deck centre/Home). Keypad indev on the focus
     * group: short press clicks the focused tile (the trackball's click), >=1s
     * hold returns to the launcher. */
    if (board->button_read) {
        s_btnIndev = lv_indev_create();
        lv_indev_set_type(s_btnIndev, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_read_cb(s_btnIndev, buttonReadCb);
        lv_indev_set_display(s_btnIndev, s_disp);
        lv_indev_set_group(s_btnIndev, s_group);
    }

    /* Optional hardware keyboard (T-Deck QWERTY). Keypad indev on the same group
     * so a focused textarea takes its keystrokes; lcdSettingText suppresses the
     * on-screen keyboard when this is present. */
    if (board->key_read) {
        s_kbIndev = lv_indev_create();
        lv_indev_set_type(s_kbIndev, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_read_cb(s_kbIndev, keyboardReadCb);
        lv_indev_set_display(s_kbIndev, s_disp);
        lv_indev_set_group(s_kbIndev, s_group);
    }

    return true;
}

int         lcdScreenW(void)     { return s_w; }
int         lcdScreenH(void)     { return s_h; }
lv_group_t* lcdInputGroup(void)  { return s_group; }
