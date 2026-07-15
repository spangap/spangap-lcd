/**
 * lcd_input.h — input HAL contract for the lcd component.
 *
 * The display itself — SPI bus, panel controller, backlight, orientation — is
 * owned by the lcd component and configured through Kconfig (CONFIG_LCD_*), so a
 * board contributes no display code. What stays board-specific is input: touch,
 * a cursor device (trackball / mouse), and a centre/Home button. A board supplies
 * these through this contract and registers it with lcdSetInput() BEFORE
 * spangapInit() (which calls lcdInit()).
 *
 * Every member is optional. A board may register nothing (or never call
 * lcdSetInput()) and the UI still comes up — navigable by a keyboard/keypad indev
 * the board joins to lcdInputGroup(). The board reads its own hardware however it
 * likes (esp_lcd_touch, raw I2C, an ADC) and reports plain coordinates; no driver
 * type crosses this interface. The component owns the LVGL indevs, the cursor,
 * press tracking and gestures.
 */
#ifndef SPANGAP_LCD_INPUT_H
#define SPANGAP_LCD_INPUT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One raw touch point in NATIVE panel coordinates (pre-orientation, the touch
 *  chip's own frame). The component applies the same CONFIG_LCD_ROTATION / mirror
 *  transform it applies to the pixels, so the board never deals with orientation. */
typedef struct { int16_t x, y; } lcd_raw_pt_t;

typedef struct {
    /** Optional one-time setup, called on the lcd task after the panel and the
     *  shared GPIO ISR service are up. Wire input INT lines and create touch
     *  handles here. May be NULL. */
    void (*init)(void);

    /** Touch read: write up to `max` raw points, set *count (0 = no finger), and
     *  return true on a successful read. Called on the lcd task when the touch
     *  INT fires and re-polled at ~10ms while a finger is down. NULL = no touch. */
    bool (*touch_read)(lcd_raw_pt_t* pts, int max, int* count);

    /** Absolute cursor device (trackball / mouse): write the current pointer
     *  position in DISPLAY pixels to *x,*y and return true iff it moved since the
     *  last call. The board owns the position — it integrates its device's motion,
     *  applies sensitivity / axis orientation, and clamps to lcdDisplaySize(). The
     *  component turns this into an LVGL pointer with an auto-hiding cursor.
     *  NULL = no cursor device. */
    bool (*pointer_read)(int* x, int* y);

    /** Centre / Home button click. Return true to assert a click — on the focused
     *  item (keypad mode) or at the cursor (pointer mode). The board owns ALL
     *  button timing: it must suppress this during a long-press and instead call
     *  lcdGoHome() itself; the component applies no hold/click policy. NULL = no
     *  button. */
    bool (*click_read)(void);
} lcd_input_t;

/** Register the input HAL. Call before spangapInit(). The pointed-to struct must
 *  outlive the process (use a static). */
void lcdSetInput(const lcd_input_t* in);

/** The final (post-orientation) display size in pixels — so a board's pointer
 *  integrator can clamp to the panel without knowing the Kconfig geometry. */
void lcdDisplaySize(int* w, int* h);

/** Shared input interrupt handler, exported by the lcd component. The board
 *  attaches this to its input INT lines (touch / button / trackball) via
 *  gpio_isr_handler_add(); it only flags + wakes the lcd task, which then reads
 *  its (event-mode) indevs on demand — no input polling. IRAM-safe (flag +
 *  vTaskNotifyGiveFromISR only); arg is ignored, so it drops straight into the
 *  gpio_isr_handler_add() signature. */
void lcdInputISR(void* arg);

/** Task-context analogue of lcdInputISR(): flag an input edge and wake the lcd
 *  task from a normal (non-ISR) context — for a board that detects input where
 *  an ISR-only notify can't run, e.g. a light-sleep wake callback (IDLE task).
 *  The lcd task then reads its (event-mode) indevs on demand, same as an edge. */
void lcdInputSignal(void);

/** Off-task touch drive. A board that samples its touch controller on its own
 *  task — to keep the read (and its I2C traffic) off the render path — calls this
 *  after latching each new sample. It is the task-safe analogue of an lcdInputISR()
 *  touch edge: it hops onto the lcd task and reads the touch indev on demand. The
 *  board still reports the sample through touch_read (which now returns its latch
 *  rather than touching hardware). Coalesced to one pending hop, so a tight sample
 *  loop can't flood the lcd task; a no-op if the board registered no touch_read. */
void lcdTouchPoll(void);

#ifdef __cplusplus
}
#endif

#endif /* SPANGAP_LCD_INPUT_H */
