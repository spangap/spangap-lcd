/**
 * lcd_panel_rgb.cpp — 16-bit RGB parallel panel bring-up from Kconfig
 * (CONFIG_LCD_RGB_*). The other half of the CONFIG_LCD_BUS choice; exactly one
 * of this file and lcd_panel.cpp compiles to anything.
 *
 * An RGB panel is not a controller with memory that you write to — it is a
 * glass that must be REFRESHED, continuously, by the SoC's LCD_CAM peripheral
 * reading a framebuffer in PSRAM and clocking it out pixel by pixel with its
 * own sync signals. So this file brings up a timing generator, not a bus, and
 * everything about it is the panel's data sheet: the porches, the pulse widths,
 * the pixel clock, the edge the glass latches on.
 *
 * WHAT IS NOT HERE: the panel controller's own registers. An ST7701S or its
 * kin still needs its gamma, power and mode registers written once before the
 * timing starts, over a side channel — usually a 3-wire SPI whose chip-select
 * the board put on an IO expander, sharing its data and clock lines with the SD
 * card. Nothing generic can be said about that channel, so the BOARD writes
 * that sequence, in its own onStart, before spangapInit() hands those pins to
 * anyone else. By the time this runs the glass is configured and waiting for
 * pixels.
 *
 * The framebuffer is the driver's own (fb_in_psram), one of them, and the flush
 * copies into it (lcd_lvgl.cpp) — see CONFIG_LCD_RGB_DRAW_LINES for the trade
 * that leaves standing.
 */
#include "sdkconfig.h"
#if CONFIG_LCD_BUS_RGB

#include "lcd_internal.h"

#include "cli.h"            /* the `panel` bring-up command */
#include "log.h"
#include "pm.h"             /* the CPU-frequency lock this transport needs */
#include "spi_helper.h"     /* spiHelperEnsureGpioIsr — the one shared GPIO ISR install */

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_rom_gpio.h"   /* `panel edge`: invert PCLK in the GPIO matrix */
#include "soc/lcd_periph.h" /* …which needs the peripheral's signal index */
#include "soc/lcd_cam_struct.h"
#include "hal/lcd_ll.h"     /* `panel fps`: the blanking, which esp_lcd fixes at create */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define BL_MODE   LEDC_LOW_SPEED_MODE
#define BL_TIMER  LEDC_TIMER_0
#define BL_CH     LEDC_CHANNEL_0

static esp_lcd_panel_handle_t s_panel        = nullptr;
static bool                   s_hasBacklight = false;
static uint16_t*              s_turn         = nullptr;   /* one strip, turned */
static uint16_t*              s_fb           = nullptr;   /* the panel's own framebuffer */

static void cliPanel(const char* args);   /* registered by the bring-up below */

/* An RGB panel scans the framebuffer out in the order the glass reads it, so
 * the hardware has no rotation to ask for. 180° is the turn that costs nothing
 * — both mirrors, which the driver does implement. A QUARTER turn is a real
 * transform, and it is done here, in the strip copy: LVGL is told the turned
 * size, renders in it, and lcdPanelBlit writes each strip into the
 * framebuffer turned (see it for the cost). The panel's own timings stay
 * native throughout — that is the glass, not the picture on it. */
/* The turn in force, read from s.lcd.rotation at bring-up and fixed for the
 * life of the boot (changing the key restarts the device — see lcd.cpp). */
static int  s_rot = CONFIG_LCD_ROTATION;
static bool rotationAsked(void) { return s_rot == 90 || s_rot == 270; }
/* Whether the quarter turn is actually being performed: it needs a strip to
 * turn the pixels into, and a board that cannot spare that PSRAM runs native
 * rather than upside down in the wrong aspect. Settled in lcdPanelInit, before
 * anything asks how big the display is. */
static bool s_turning = false;
/* Native: what the timing generator scans, and what a raw touch point is in. */
static int panelW(void) { return CONFIG_LCD_NATIVE_WIDTH;  }
static int panelH(void) { return CONFIG_LCD_NATIVE_HEIGHT; }
/* Display: what LVGL draws in. The two differ by a quarter turn, or not at all. */
static int dispW(void) { return s_turning ? panelH() : panelW(); }
static int dispH(void) { return s_turning ? panelW() : panelH(); }

/* Backlight: identical to the SPI panel's, because it is not part of either
 * transport — it is one LEDC channel on one pin. RC_FAST so the PWM keeps
 * toggling through light sleep; the channel is configured exactly once, since
 * re-running channel_config re-reserves the GPIO and logs a conflict each
 * time. */
static void backlightInit(void) {
#if CONFIG_LCD_BL_EN_PIN >= 0
    gpio_reset_pin((gpio_num_t)CONFIG_LCD_BL_EN_PIN);
    gpio_set_direction((gpio_num_t)CONFIG_LCD_BL_EN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)CONFIG_LCD_BL_EN_PIN, 0);
#endif
    if (CONFIG_LCD_BL_PIN < 0) return;
    ledc_timer_config_t t = {};
    t.speed_mode      = BL_MODE;
    t.duty_resolution = LEDC_TIMER_8_BIT;       /* 0..255 */
    t.timer_num       = BL_TIMER;
    t.freq_hz         = 5000;
    t.clk_cfg         = LEDC_USE_RC_FAST_CLK;
    ledc_timer_config(&t);

    ledc_channel_config_t c = {};
    c.gpio_num   = CONFIG_LCD_BL_PIN;
    c.speed_mode = BL_MODE;
    c.channel    = BL_CH;
    c.timer_sel  = BL_TIMER;
    c.hpoint     = 0;
    c.duty       = 0;                            /* start dark */
    c.sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE;
#if CONFIG_LCD_BL_ACTIVE_LOW
    c.flags.output_invert = 1;
#endif
    ledc_channel_config(&c);
    s_hasBacklight = true;
}

void lcdPanelBacklight(uint8_t level) {
#if CONFIG_LCD_BL_EN_PIN >= 0
    gpio_set_level((gpio_num_t)CONFIG_LCD_BL_EN_PIN, level > 0);
#endif
    if (!s_hasBacklight) return;
    /* 255, not 2^8: the full-scale overflow value is a latch rather than a duty
     * register and is not retained across a KEEP_ALIVE light sleep. */
    ledc_set_duty(BL_MODE, BL_CH, level);
    ledc_update_duty(BL_MODE, BL_CH);
}

/* ── the CPU clock, while the glass is being looked at ──────────────────────
 * An RGB panel is refreshed out of PSRAM, forever, by a DMA that cannot be
 * late — and it shares that memory with a CPU whose clock this platform scales
 * down to 80 MHz whenever nothing holds it up. A slow CPU does not make the DMA
 * slow, but it makes every cache miss it takes occupy the memory for longer,
 * and the panel is what waits. What that looks like is a picture that was fine
 * for the first few seconds after boot and is not fine afterwards, which is the
 * least diagnosable symptom on this list: nothing changed except that the
 * device stopped being busy.
 *
 * ESP-IDF's own driver takes a CPU-frequency lock for exactly this reason on
 * the P4 and, on this chip, only a no-light-sleep one. So the lock is taken
 * here — and released in standby, because a screen nobody is looking at can
 * scan badly for free. That is the whole cost: full speed while the backlight
 * is up, scaling as usual while it is down. */
static pm_lock_handle_t s_pmLock   = nullptr;
static bool             s_pmHeld   = false;
static bool             s_pmWanted = false;   /* `panel cpu 1`; off by default */

static void panelHoldCpu(bool hold) {
    if (!s_pmLock || hold == s_pmHeld) return;
    if (hold) pmLockAcquire(s_pmLock); else pmLockRelease(s_pmLock);
    s_pmHeld = hold;
}

/* The inactivity blank. What the operator sees is decided by the backlight,
 * which lcd.cpp drives to 0 alongside this — the glass holds nothing of its
 * own. This call drives the panel's enable LINE, and the bring-up below hands
 * esp_lcd no such line (disp_gpio_num = -1), so it answers "not supported" and
 * the scan runs on through standby — the LCD DMA reading a framebuffer nobody
 * is looking at. Stopping it is the one power saving this transport still has
 * to give, and it needs that pin. */
void lcdPanelDisplayPower(bool on) {
    panelHoldCpu(on && s_pmWanted);
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, on);
}

esp_lcd_panel_handle_t lcdPanelInit(esp_lcd_panel_io_handle_t* ioOut, int* wOut, int* hOut) {
    s_rot = lcdRotationSetting();      /* before anything asks how big the display is */

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src       = LCD_CLK_SRC_DEFAULT;
    cfg.data_width    = 16;                 /* RGB565 across sixteen lines */
    cfg.bits_per_pixel = 16;
    cfg.num_fbs       = 1;
    cfg.dma_burst_size = 64;
    cfg.hsync_gpio_num = CONFIG_LCD_RGB_HSYNC_PIN;
    cfg.vsync_gpio_num = CONFIG_LCD_RGB_VSYNC_PIN;
    cfg.de_gpio_num    = CONFIG_LCD_RGB_DE_PIN;
    cfg.pclk_gpio_num  = CONFIG_LCD_RGB_PCLK_PIN;
    cfg.disp_gpio_num  = -1;                /* no panel-enable line brought out */
    /* data_gpio_nums is indexed by BIT of the 16-bit bus, and the bus is
     * RGB565 packed [R4..R0 G5..G0 B4..B0] — so blue occupies bits 0-4, green
     * 5-10 and red 11-15. Writing it out in that order is what makes a board's
     * pin block a transcription of its schematic rather than an arithmetic. */
    cfg.data_gpio_nums[0]  = CONFIG_LCD_RGB_B0_PIN;
    cfg.data_gpio_nums[1]  = CONFIG_LCD_RGB_B1_PIN;
    cfg.data_gpio_nums[2]  = CONFIG_LCD_RGB_B2_PIN;
    cfg.data_gpio_nums[3]  = CONFIG_LCD_RGB_B3_PIN;
    cfg.data_gpio_nums[4]  = CONFIG_LCD_RGB_B4_PIN;
    cfg.data_gpio_nums[5]  = CONFIG_LCD_RGB_G0_PIN;
    cfg.data_gpio_nums[6]  = CONFIG_LCD_RGB_G1_PIN;
    cfg.data_gpio_nums[7]  = CONFIG_LCD_RGB_G2_PIN;
    cfg.data_gpio_nums[8]  = CONFIG_LCD_RGB_G3_PIN;
    cfg.data_gpio_nums[9]  = CONFIG_LCD_RGB_G4_PIN;
    cfg.data_gpio_nums[10] = CONFIG_LCD_RGB_G5_PIN;
    cfg.data_gpio_nums[11] = CONFIG_LCD_RGB_R0_PIN;
    cfg.data_gpio_nums[12] = CONFIG_LCD_RGB_R1_PIN;
    cfg.data_gpio_nums[13] = CONFIG_LCD_RGB_R2_PIN;
    cfg.data_gpio_nums[14] = CONFIG_LCD_RGB_R3_PIN;
    cfg.data_gpio_nums[15] = CONFIG_LCD_RGB_R4_PIN;

    cfg.timings.pclk_hz           = (unsigned)CONFIG_LCD_PCLK_MHZ * 1000 * 1000;
    cfg.timings.h_res             = panelW();
    cfg.timings.v_res             = panelH();
    cfg.timings.hsync_pulse_width = CONFIG_LCD_RGB_HSYNC_PULSE;
    cfg.timings.hsync_back_porch  = CONFIG_LCD_RGB_HSYNC_BACK;
    cfg.timings.hsync_front_porch = CONFIG_LCD_RGB_HSYNC_FRONT;
    cfg.timings.vsync_pulse_width = CONFIG_LCD_RGB_VSYNC_PULSE;
    cfg.timings.vsync_back_porch  = CONFIG_LCD_RGB_VSYNC_BACK;
    cfg.timings.vsync_front_porch = CONFIG_LCD_RGB_VSYNC_FRONT;
    /* An unset Kconfig bool is an UNDEFINED macro, not a zero, so this is a
     * preprocessor question rather than an expression. */
#if CONFIG_LCD_RGB_PCLK_ACTIVE_NEG
    cfg.timings.flags.pclk_active_neg = 1;
#endif

    /* The framebuffer lives in PSRAM — 600 KB at 480x640 is not internal RAM's
     * to give — and the LCD DMA reads it directly unless bounce buffers are
     * asked for. Bounce buffers are the answer to the one moment that DMA
     * cannot have the memory bus: a flash write, which on this SoC parks the
     * cache and, with a straight-from-PSRAM panel, draws a band of noise across
     * the glass while it lasts. */
    cfg.flags.fb_in_psram = 1;
    cfg.bounce_buffer_size_px = (size_t)CONFIG_LCD_RGB_BOUNCE_LINES * panelW();

    if (esp_lcd_new_rgb_panel(&cfg, &s_panel) != ESP_OK) {
        err("rgb panel init failed\n");
        return nullptr;
    }
    if (esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel)  != ESP_OK) {
        err("rgb panel bring-up failed\n");
        return nullptr;
    }

    /* The turned strip, staged before it goes to the framebuffer: one LVGL
     * draw buffer's worth, which is the most a single flush can carry. PSRAM —
     * it is written once in order and read once by the copy that follows.
     * Without it a quarter turn has nowhere to happen, so the display falls
     * back to native and says so rather than showing a scrambled screen. */
    if (rotationAsked()) {
        /* Turned, the display is as wide as the panel is tall. */
        const size_t bytes = (size_t)panelH() * CONFIG_LCD_RGB_DRAW_LINES * sizeof(uint16_t);
        s_turn    = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        s_turning = (s_turn != nullptr);
        if (!s_turning)
            err("rotation %d needs a %u B PSRAM strip it cannot have — showing it native\n",
                s_rot, (unsigned)bytes);
    }
    /* 180° is both mirrors; the Kconfig toggles XOR on top of that for a glass
     * whose scan direction differs from its data sheet. */
    bool mx = (s_rot == 180), my = mx;
#if CONFIG_LCD_MIRROR_X
    mx = !mx;
#endif
#if CONFIG_LCD_MIRROR_Y
    my = !my;
#endif
    if ((mx || my) && esp_lcd_panel_mirror(s_panel, mx, my) != ESP_OK)
        warn("rgb panel would not mirror (x=%d y=%d)\n", mx, my);

    esp_lcd_panel_disp_on_off(s_panel, true);

    backlightInit();

    /* Shared GPIO ISR service for the board's input INT lines (touch / button),
     * installed here so it exists before the board's input init runs — same
     * contract as the SPI panel's bring-up. */
    spiHelperEnsureGpioIsr(ESP_INTR_FLAG_IRAM);

    cliRegisterCmd("panel", cliPanel);

    /* The turned strip goes straight into the framebuffer, which halves the
     * memory it crosses — but only where the panel reads that memory with the
     * CPU, i.e. through bounce buffers, so a cached write is a write it sees.
     * Without them the DMA reads PSRAM directly and the copy has to go through
     * esp_lcd, which owns the cache sync. */
    if (s_turning && CONFIG_LCD_RGB_BOUNCE_LINES > 0) {
        void* fb = nullptr;
        if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb) == ESP_OK)
            s_fb = (uint16_t*)fb;
    }

    /* Created, not held. The refill is a CPU copy against a deadline, so a
     * processor free to drop to 80 MHz is a real risk to it — but this panel
     * meets that deadline at 80 MHz, and pinning the clock for as long as a
     * screen is on is a cost paid every second of every day against a fault
     * that does not occur. `panel cpu 1` takes it, for a board or a build where
     * it does. */
    pmLockCreate(PM_CPU_FREQ_MAX, "rgb_panel", &s_pmLock);

    /* Both sizes, because on a turned display they differ and each answers a
     * different question: the glass is what the timings drive, the display is
     * what LVGL draws in. */
    info("rgb panel %dx%d @ %d MHz, display %dx%d at %d%s\n",
         panelW(), panelH(), CONFIG_LCD_PCLK_MHZ, dispW(), dispH(), s_rot,
         CONFIG_LCD_RGB_BOUNCE_LINES > 0 ? ", bounced" : "");

    /* No panel-io: an RGB panel has no command channel to hand back, and
     * lcd_lvgl.cpp's colour-transfer-done callback belongs to the SPI path. */
    if (ioOut) *ioOut = nullptr;
    if (wOut)  *wOut  = dispW();
    if (hOut)  *hOut  = dispH();
    return s_panel;
}

/* One rendered strip into the framebuffer, turned if the display is held at a
 * quarter turn. `area` is in display coordinates, `px` is LVGL's draw buffer.
 *
 * Untransformed this is one call: esp_lcd's RGB draw_bitmap is a row-by-row
 * copy into the memory the LCD DMA is already scanning out. A quarter turn
 * makes it a transpose first, and a transpose is the one copy shape a cache
 * hates — a column of the source is a column of cache lines, one useful pixel
 * in each. So it walks the strip in 16x16 TILES: every line a tile pulls in is
 * read sixteen times before it leaves, which is the difference between a
 * full-screen repaint costing tens of milliseconds and costing a fifth of a
 * second. The turned tile lands in s_turn, laid out as the panel reads it, and
 * the ordinary draw_bitmap takes it from there. */
/* A test pattern owns the glass until something is pressed. It is written
 * straight into the framebuffer, under LVGL rather than in it — which is the
 * point, since what these patterns test is the path LVGL's own pixels take —
 * so the only way to keep the UI from painting over it is to stop the UI's
 * pixels reaching the panel. That is this flag: LVGL goes on rendering into its
 * draw buffer and every flush is dropped on the floor, so nothing about the
 * running UI changes and nothing of it is seen. The first touch puts it back. */
static bool s_patternUp = false;

bool lcdPanelPatternUp(void) { return s_patternUp; }

void lcdPanelPatternClear(void) {
    if (!s_patternUp) return;
    s_patternUp = false;
    /* LVGL believes the screen already holds what it last drew, and none of it
     * is there. Only a full invalidation gets all of it back. */
    lcdRun(ON_LCD { lv_obj_invalidate(lv_screen_active()); });
}

static void blitTurned(const lv_area_t* area, const void* px);

void lcdPanelBlit(const lv_area_t* area, const void* px) {
    if (s_patternUp) return;
    blitTurned(area, px);
}

static void blitTurned(const lv_area_t* area, const void* px) {
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    if (!s_turning) {
        esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                                  area->x2 + 1, area->y2 + 1, px);
        return;
    }

    const uint16_t* src = (const uint16_t*)px;
    /* Display (x,y) → panel (px,py), the turn LVGL renders against:
     *    90°:  px = (PW-1) - y,  py = x       270°:  px = y,  py = (PH-1) - x
     * so a w x h strip becomes an h x w one, and the destination row (a panel
     * row) is a display COLUMN. Indices below are strip-relative:
     * i = x - x1 (0..w-1), j = y - y1 (0..h-1). */
    const bool cw = (s_rot == 90);
    const int  TILE = 16;
    const int  PW = panelW(), PH = panelH();
    const int  ax = area->x1, ay = area->y1;

    if (s_fb) {
        for (int i0 = 0; i0 < w; i0 += TILE) {
            const int in = (i0 + TILE < w) ? i0 + TILE : w;
            for (int j0 = 0; j0 < h; j0 += TILE) {
                const int jn = (j0 + TILE < h) ? j0 + TILE : h;
                for (int i = i0; i < in; i++) {
                    const uint16_t* s = src + (size_t)j0 * w + i;
                    uint16_t* d = cw
                        ? s_fb + (size_t)(ax + i) * PW + ((PW - 1) - (ay + j0))
                        : s_fb + (size_t)((PH - 1) - (ax + i)) * PW + (ay + j0);
                    const int step = cw ? -1 : 1;
                    for (int j = j0; j < jn; j++) { *d = *s; s += w; d += step; }
                }
            }
        }
        return;
    }

    for (int j0 = 0; j0 < h; j0 += TILE) {
        const int jn = (j0 + TILE < h) ? j0 + TILE : h;
        for (int i0 = 0; i0 < w; i0 += TILE) {
            const int in = (i0 + TILE < w) ? i0 + TILE : w;
            for (int j = j0; j < jn; j++) {
                const uint16_t* s = src + (size_t)j * w + i0;
                /* Row within the turned strip, and the step along it. */
                uint16_t* d = cw ? s_turn + (size_t)i0 * h + (h - 1 - j)
                                 : s_turn + (size_t)(w - 1 - i0) * h + j;
                const int step = cw ? h : -h;
                for (int i = i0; i < in; i++) { *d = *s++; d += step; }
            }
        }
    }

    const int x1 = cw ? (PW - 1) - area->y2 : area->y1;
    const int y1 = cw ? area->x1            : (PH - 1) - area->x2;
    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x1 + h, y1 + w, s_turn);
}

/* `panel` — the RGB timing generator, and the two knobs that decide whether a
 * picture is steady, both live.
 *
 * An unsteady RGB picture is a data line the glass did not read cleanly, and
 * there are only a few ways that happens: the clock is faster than the wiring
 * can carry, or the pins are driving it too softly to settle before the glass
 * latches. Neither can be reasoned out from here — they are a property of one
 * board's traces and one panel's input stage — so both are adjustable while
 * looking at the screen, and the answer is then written into the board's
 * straddle.yaml where it belongs. The clock takes effect at the next vsync; the
 * drive strength on the next pixel. Nothing here is stored: a reboot is back to
 * what the board says. */
static const int kPanelPins[] = {
    CONFIG_LCD_RGB_HSYNC_PIN, CONFIG_LCD_RGB_VSYNC_PIN,
    CONFIG_LCD_RGB_DE_PIN,    CONFIG_LCD_RGB_PCLK_PIN,
    CONFIG_LCD_RGB_B0_PIN, CONFIG_LCD_RGB_B1_PIN, CONFIG_LCD_RGB_B2_PIN,
    CONFIG_LCD_RGB_B3_PIN, CONFIG_LCD_RGB_B4_PIN,
    CONFIG_LCD_RGB_G0_PIN, CONFIG_LCD_RGB_G1_PIN, CONFIG_LCD_RGB_G2_PIN,
    CONFIG_LCD_RGB_G3_PIN, CONFIG_LCD_RGB_G4_PIN, CONFIG_LCD_RGB_G5_PIN,
    CONFIG_LCD_RGB_R0_PIN, CONFIG_LCD_RGB_R1_PIN, CONFIG_LCD_RGB_R2_PIN,
    CONFIG_LCD_RGB_R3_PIN, CONFIG_LCD_RGB_R4_PIN,
};
static int s_pclkMhz  = CONFIG_LCD_PCLK_MHZ;
static int s_driveCap = -1;      /* -1 = whatever the GPIO driver starts pins at */
/* An unset Kconfig bool is an UNDEFINED macro, so the edge has to be a
 * preprocessor question here as it is at bring-up. */
#if CONFIG_LCD_RGB_PCLK_ACTIVE_NEG
static const bool kPclkNeg = true;
#else
static const bool kPclkNeg = false;
#endif
static bool s_edgeNeg = kPclkNeg;   /* what `panel edge` has made of it since */

static void cliPanel(const char* args) {
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s RGB panel: test card, live pclk/drive, timing\n",
                  CLI_HELP_COL, "panel [test|pclk|drive|restart|fill|bits|restore]");
        if (args && args[0] == '-') {          /* -h / --help: the long form */
            cliPrintf("%-*s the test card: bars, ramps, 1px grids, data lines\n",
                      CLI_HELP_COL, "panel test");
            cliPrintf("%-*s which clock edge the glass latches on (ghosting)\n",
                      CLI_HELP_COL, "panel edge <0|1>");
            cliPrintf("%-*s begin a frame cleanly: the way back from lost sync\n",
                      CLI_HELP_COL, "panel restart");
            cliPrintf("%-*s pixel clock, from the next vsync\n",
                      CLI_HELP_COL, "panel pclk <MHz>");
            cliPrintf("%-*s frame rate, by stretching the blanking\n",
                      CLI_HELP_COL, "panel fps <n>");
            cliPrintf("%-*s hold the CPU at max while the screen is on\n",
                      CLI_HELP_COL, "panel cpu <0|1>");
            cliPrintf("%-*s drive strength of all 20 pins (2 = default)\n",
                      CLI_HELP_COL, "panel drive <0-3>");
            cliPrintf("%-*s flood the glass with one RGB565 value\n",
                      CLI_HELP_COL, "panel fill <hex>");
            cliPrintf("%-*s one stripe per data line, bit 0 at the left\n",
                      CLI_HELP_COL, "panel bits");
            cliPrintf("%-*s hand the screen back to the UI\n",
                      CLI_HELP_COL, "panel restore");
        }
        return;
    }
    unsigned v = 0;
    if (args && sscanf(args, "pclk %u", &v) == 1) {
        if (v < 2 || v > 30) { cliPrintf("pclk 2..30 MHz\n"); return; }
        if (esp_lcd_rgb_panel_set_pclk(s_panel, v * 1000000) != ESP_OK) {
            cliPrintf("pclk %u MHz refused\n", v);
            return;
        }
        s_pclkMhz = (int)v;
        /* The change lands mid-frame, and the frame it lands in is malformed —
         * which is exactly the kind of frame a panel stops following. So the
         * transmission is restarted behind it, every time: without that, a
         * clock sweep leaves the glass out of lock at some step and every
         * later step reads as "this value is bad too". */
        esp_lcd_rgb_panel_restart(s_panel);
        cliPrintf("pclk %d MHz, transmission restarted\n", s_pclkMhz);
        return;
    }
    /* Frame rate, by stretching the blanking rather than slowing the clock.
     *
     * The two are not the same lever. The pixel clock has a floor the panel
     * sets — below it the glass stops locking — while the bandwidth the
     * framebuffer costs is pixels per SECOND, which the blanking intervals
     * lower without the clock going anywhere near that floor. The blanking is
     * time the panel spends between lines and between frames carrying no
     * pixels at all.
     *
     * The registers bound it: a 10-bit vertical total (1024 rows) and a 12-bit
     * horizontal one (4096 clocks per line). The vertical is stretched first —
     * a long gap between frames is what every panel already sees — and the line
     * is only lengthened when that is not enough. */
    if (args && sscanf(args, "fps %u", &v) == 1) {
        if (v < 1) { cliPrintf("fps 1..60\n"); return; }
        const int VT_MAX = 1024, HT_MAX = 4096;
        const int hAct = panelW(), vAct = panelH();
        const int hsw = CONFIG_LCD_RGB_HSYNC_PULSE, hbp = CONFIG_LCD_RGB_HSYNC_BACK;
        const int vsw = CONFIG_LCD_RGB_VSYNC_PULSE, vbp = CONFIG_LCD_RGB_VSYNC_BACK;
        const int htMin = hAct + hsw + hbp + CONFIG_LCD_RGB_HSYNC_FRONT;
        const int vtMin = vAct + vsw + vbp + CONFIG_LCD_RGB_VSYNC_FRONT;
        const long total = (long)s_pclkMhz * 1000000 / (long)v;

        int vt = VT_MAX, ht = (int)(total / vt);
        if (ht < htMin) { ht = htMin; vt = (int)(total / ht); }
        if (ht > HT_MAX) ht = HT_MAX;
        if (vt > VT_MAX) vt = VT_MAX;
        if (vt < vtMin)  vt = vtMin;

        lcd_ll_set_horizontal_timing(&LCD_CAM, hsw, hbp, hAct, ht - hsw - hbp - hAct);
        lcd_ll_set_vertical_timing(&LCD_CAM, vsw, vbp, vAct, vt - vsw - vbp - vAct);
        esp_lcd_rgb_panel_restart(s_panel);
        const int got = (int)((long)s_pclkMhz * 1000000 / ((long)ht * vt));
        cliPrintf("%d fps: %dx%d clocks per frame (%d active), %d MHz\n",
                  got, ht, vt, hAct, s_pclkMhz);
        cliPrintf("framebuffer read %d KB/s\n",
                  (int)((long)hAct * vAct * 2 * got / 1024));
        return;
    }

    /* The CPU-frequency lock, live. Held, the processor stays at its maximum
     * for as long as the screen is on, because the bounce refill is a CPU copy
     * against a deadline and at 80 MHz it has a third of the speed to make it
     * in. Whether it is still needed is a question about how much else is
     * competing, which changes as the rest of the device does — so it is a
     * switch rather than a belief. Standby takes it back either way. */
    if (args && sscanf(args, "cpu %u", &v) == 1) {
        s_pmWanted = (v != 0);
        panelHoldCpu(s_pmWanted);
        cliPrintf("cpu pinned at max: %s\n", s_pmHeld ? "yes" : "no (free to scale)");
        return;
    }

    /* Which clock edge the glass latches the bus on, live.
     *
     * The peripheral's own setting for this is fixed when the panel is created,
     * so this does the same thing one step further out: it inverts the pixel
     * clock where it leaves the chip, in the GPIO matrix. The panel then sees
     * its clock the other way up, which is the whole of what the setting means
     * to it, and the data lines are untouched.
     *
     * It is the knob for GHOSTING — a pixel wearing a trace of the one before
     * it. That is the glass sampling the bus while it is still moving, and
     * moving the sampling edge is the only thing that addresses it directly; a
     * slower clock only makes the same mistake later. Whichever way round it
     * comes out clean is what the board writes into
     * CONFIG_LCD_RGB_PCLK_ACTIVE_NEG, since that is the same choice made where
     * it belongs. */
    if (args && sscanf(args, "edge %u", &v) == 1) {
        const bool neg = (v != 0);
        esp_rom_gpio_connect_out_signal(CONFIG_LCD_RGB_PCLK_PIN,
                                        lcd_periph_rgb_signals.panels[0].pclk_sig,
                                        neg, false);
        s_edgeNeg = neg;
        esp_lcd_rgb_panel_restart(s_panel);
        cliPrintf("latching on the %s edge\n", neg ? "falling" : "rising");
        return;
    }
    /* The way back from a picture that has come unstuck. An RGB panel that
     * stops following the sync does not start again on its own: the SoC keeps
     * clocking out a perfectly good frame and the glass keeps drawing it in the
     * wrong place, or unsteadily, for as long as it is left alone. This resets
     * the DMA channel and begins a frame cleanly, which is what lets it lock
     * again — and it is the first thing to try before believing a timing value
     * is bad. */
    if (args && !strncmp(args, "restart", 7)) {
        cliPrintf(esp_lcd_rgb_panel_restart(s_panel) == ESP_OK
                  ? "transmission restarted\n" : "restart refused\n");
        return;
    }
    if (args && sscanf(args, "drive %u", &v) == 1) {
        if (v > 3) { cliPrintf("drive 0..3 (0 = weakest, 2 = the default, 3 = strongest)\n"); return; }
        for (size_t i = 0; i < sizeof(kPanelPins) / sizeof(kPanelPins[0]); i++)
            if (kPanelPins[i] >= 0)
                gpio_set_drive_capability((gpio_num_t)kPanelPins[i], (gpio_drive_cap_t)v);
        s_driveCap = (int)v;
        cliPrintf("drive %d on 20 pins\n", s_driveCap);
        return;
    }
    /* Test patterns, written straight into the framebuffer in PANEL
     * coordinates — no LVGL, no rotation, no theme. What they are for is the
     * one question a schematic cannot answer: which of the panel's colour bits
     * each of the SoC's sixteen data lines actually reaches. `bits` lights one
     * line per vertical stripe, bit 0 at the left edge of the glass as the
     * ribbon comes out of it, so reading the stripes left to right reads the
     * wiring: five blue, six green, five red is the mapping this driver assumes
     * (RGB565, blue in the low bits). Anything else — a stripe in the wrong
     * colour, a channel that never reaches full brightness — is the panel
     * reading its bus at a different width or offset than COLMOD says.
     * `fill` takes one RGB565 value, for checking a channel end to end.
     *
     * Nothing is restored: the next repaint is whatever LVGL draws over it, and
     * `panel restore` asks for that repaint. */
    if (args && (sscanf(args, "fill %x", &v) == 1 || !strncmp(args, "bits", 4))) {
        const bool bits = (args[0] == 'b');
        const int  W = panelW(), H = panelH();
        uint16_t*  row = (uint16_t*)heap_caps_malloc((size_t)W * 2, MALLOC_CAP_DEFAULT);
        if (!row) { cliPrintf("no memory for a row\n"); return; }
        s_patternUp = true;      /* before the first row: the UI stops here */
        for (int x = 0; x < W; x++)
            row[x] = bits ? (uint16_t)(1u << ((x * 16) / W)) : (uint16_t)v;
        for (int y = 0; y < H; y++) esp_lcd_panel_draw_bitmap(s_panel, 0, y, W, y + 1, row);
        free(row);
        if (bits) cliPrintf("16 stripes, one data line each, bit 0 at the left of the glass.\n");
        else      cliPrintf("filled with 0x%04X.\n", v);
        cliPrintf("Touch the screen to put the UI back.\n");
        return;
    }
    /* The test card. One screen, drawn in DISPLAY coordinates so it comes out
     * the way the operator is holding the device, and every band answers a
     * different question about the wiring between the framebuffer and the
     * glass:
     *
     *   colour bars    each channel on its own and in pairs — a bar in the
     *                  wrong colour is a channel reaching the wrong pins
     *   three ramps    one channel each, dark to full: a ramp that steps in
     *                  visible blocks has lost its low bits, one that stops
     *                  short has lost its high ones
     *   grey ramp      all three together — grey is the only colour that shows
     *                  a channel imbalance as a TINT, which is what "white
     *                  looks purple" means
     *   1px columns    alternate black and white pixels across: the worst case
     *                  for the pixel clock, and where an unsteady picture
     *                  shimmers first
     *   1px rows       the same test down the screen, which the pixel clock
     *                  cannot explain — if only the columns shimmer it is the
     *                  clock or the drive, if both do it is the frame timing
     *   16 stripes     one data line each, low bit first: five blue, six
     *                  green, five red is what this driver is sending
     */
    if (args && !strncmp(args, "test", 4)) {
        const int W = dispW(), H = dispH();
        uint16_t* row = (uint16_t*)heap_caps_malloc((size_t)W * 2, MALLOC_CAP_DEFAULT);
        if (!row) { cliPrintf("no memory for a row\n"); return; }
        s_patternUp = true;      /* before the first row: the UI stops here */
        static const uint16_t bars[8] = { 0x0000, 0x001F, 0x07E0, 0x07FF,
                                          0xF800, 0xF81F, 0xFFE0, 0xFFFF };
        const int b1 = H / 6, b2 = b1 + H / 12, b3 = b2 + H / 12, b4 = b3 + H / 12,
                  b5 = b4 + H / 8, b6 = b5 + H / 10, b7 = b6 + H / 10;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                uint16_t px;
                if      (y < b1) px = bars[(x * 8) / W];
                else if (y < b2) px = (uint16_t)((x * 32 / W) << 11);
                else if (y < b3) px = (uint16_t)((x * 64 / W) << 5);
                else if (y < b4) px = (uint16_t)(x * 32 / W);
                else if (y < b5) { int g = x * 32 / W;
                                   px = (uint16_t)((g << 11) | ((g * 2) << 5) | g); }
                else if (y < b6) px = (x & 1) ? 0xFFFF : 0x0000;
                else if (y < b7) px = (y & 1) ? 0xFFFF : 0x0000;
                else             px = (uint16_t)(1u << ((x * 16) / W));
                row[x] = px;
            }
            lv_area_t a;
            a.x1 = 0; a.y1 = y; a.x2 = W - 1; a.y2 = y;
            blitTurned(&a, row);        /* past the guard: this IS the pattern */
        }
        free(row);
        cliPrintf("test card: bars, r/g/b ramps, grey ramp, 1px columns,\n"
                  "1px rows, then 16 data-line stripes (bit 0 at the left).\n"
                  "Touch the screen to put the UI back.\n");
        return;
    }
    if (args && !strncmp(args, "restore", 7)) {
        lcdPanelPatternClear();
        cliPrintf("repainting\n");
        return;
    }

    cliPrintf("glass %dx%d, display %dx%d turned %d deg (s.lcd.rotation)\n",
              panelW(), panelH(), dispW(), dispH(), s_rot);
    cliPrintf("pclk %d MHz, %s edge, h %d/%d/%d, v %d/%d/%d\n", s_pclkMhz,
              s_edgeNeg ? "falling" : "rising",
              CONFIG_LCD_RGB_HSYNC_PULSE, CONFIG_LCD_RGB_HSYNC_BACK, CONFIG_LCD_RGB_HSYNC_FRONT,
              CONFIG_LCD_RGB_VSYNC_PULSE, CONFIG_LCD_RGB_VSYNC_BACK, CONFIG_LCD_RGB_VSYNC_FRONT);
    cliPrintf("draw strip %d lines, bounce %d lines, drive %d, cpu pinned %s\n",
              CONFIG_LCD_RGB_DRAW_LINES, CONFIG_LCD_RGB_BOUNCE_LINES, s_driveCap,
              s_pmHeld ? "yes" : "no");
}

/* Raw native (touch-chip-frame) coordinates → display coordinates: the turn the
 * pixels took, then the mirrors and the clamp. Same table as the SPI panel's,
 * which gets its turn from the controller rather than from the copy above. */
void lcdPanelOrientTouch(int rawX, int rawY, int* outX, int* outY) {
    const int NW = panelW(), NH = panelH();
    int x = rawX, y = rawY;
    if (s_turning) {
        if (s_rot == 90) { x = rawY;            y = (NW - 1) - rawX; }
        else             { x = (NH - 1) - rawY; y = rawX;            }
    } else if (s_rot == 180) {
        x = (NW - 1) - rawX; y = (NH - 1) - rawY;
    }
    const int W = dispW(), H = dispH();
#if CONFIG_LCD_MIRROR_X
    x = (W - 1) - x;
#endif
#if CONFIG_LCD_MIRROR_Y
    y = (H - 1) - y;
#endif
    if (x < 0) x = 0; else if (x >= W) x = W - 1;
    if (y < 0) y = 0; else if (y >= H) y = H - 1;
    if (outX) *outX = x;
    if (outY) *outY = y;
}

#endif  /* CONFIG_LCD_BUS_RGB */
