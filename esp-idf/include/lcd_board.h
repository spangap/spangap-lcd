/**
 * lcd_board.h — display / input HAL contract for the lcd component.
 *
 * The consumer's board layer (e.g. reticulous/main/tdeck.cpp) fills in an
 * lcd_board_t with its hardware bring-up functions and registers it via
 * lcdSetBoard() BEFORE spangapInit() (which calls lcdInit()). The lcd component
 * calls through these pointers — so all wiring (SPI pins, panel controller,
 * backlight GPIO, touch bus) lives behind this contract, and there is no
 * link-time dependency from spangap-core back onto consumer symbols. If no
 * board is registered, lcdInit() logs and bails (no display brought up).
 */
#ifndef SPANGAP_LCD_BOARD_H
#define SPANGAP_LCD_BOARD_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Bring up the panel: init the (shared) SPI bus, panel IO, controller, set
     *  orientation, leave it on. Returns the panel handle and, via the out
     *  params, the panel-IO handle (lcd registers its DMA-done callback on it)
     *  and the resolution in the chosen orientation. NULL on failure. */
    esp_lcd_panel_handle_t (*init)(esp_lcd_panel_io_handle_t* ioOut,
                                   int* wOut, int* hOut);
    /** Power the panel down (sleep). May be NULL. */
    void                   (*shutdown)(void);
    /** Backlight 0..255 (0 = off). May be NULL. */
    void                   (*backlight)(uint8_t level);
    /** Optional: put the panel into / out of low-power standby (display off,
     *  GRAM retained for an instant resume). The lcd component calls this around
     *  the inactivity blank, in addition to backlight(0): on=false → standby,
     *  on=true → resume. NULL = panel stays on and only the backlight is cut. */
    void                   (*display_power)(bool on);
    /** Bring up touch; return NULL if the board has no touch (lcd then runs
     *  without a pointer indev). The field itself may also be NULL. */
    esp_lcd_touch_handle_t (*touch_init)(void);
    /** Optional hardware button: return true while the centre/Home button is
     *  held. NULL = no button. A short press clicks the focused item; a hold of
     *  >=300ms returns to the launcher. */
    bool                   (*button_read)(void);
    /** Optional cursor device (trackball / mouse): write the current absolute
     *  pointer position in screen pixels to *x,*y and return true iff it moved
     *  since the last call. The board owns the position — it integrates its own
     *  device's motion, applies sensitivity + axis orientation, and clamps to the
     *  panel. lcd turns this into an LVGL pointer with an auto-hiding cursor, and
     *  uses button_read() as the click. NULL = no cursor device. When set, lcd
     *  routes the centre button to this pointer instead of a keypad indev. */
    bool                   (*pointer_read)(int* x, int* y);
} lcd_board_t;

/** Register the board HAL. Call before spangapInit(). The pointed-to struct
 *  must outlive the process (use a static). */
void lcdSetBoard(const lcd_board_t* board);

/** Shared input interrupt handler, exported by the lcd component. The board
 *  attaches this to its input INT lines (touch / button / trackball) via
 *  gpio_isr_handler_add(); it does nothing but flag + wake the lcd task, which
 *  then reads its (event-mode) indevs on demand — no input polling. IRAM-safe
 *  (flag + vTaskNotifyGiveFromISR only, no flash/PSRAM); arg is ignored, so it
 *  drops straight into the gpio_isr_handler_add() signature. */
void lcdInputISR(void* arg);

#ifdef __cplusplus
}
#endif

#endif /* SPANGAP_LCD_BOARD_H */
