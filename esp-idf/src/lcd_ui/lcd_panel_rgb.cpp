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

#include "log.h"
#include "spi_helper.h"     /* spiHelperEnsureGpioIsr — the one shared GPIO ISR install */

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"

#include <cstdint>

#define BL_MODE   LEDC_LOW_SPEED_MODE
#define BL_TIMER  LEDC_TIMER_0
#define BL_CH     LEDC_CHANNEL_0

static esp_lcd_panel_handle_t s_panel        = nullptr;
static bool                   s_hasBacklight = false;

/* An RGB panel scans the framebuffer out in the order the glass reads it, so
 * there is no rotation to ask the hardware for: the display size IS the native
 * size. 180° is the one turn that costs nothing (both mirrors, which the driver
 * does implement); the quarter turns would be a software rotate of every frame
 * and are refused at bring-up instead. */
static bool rotationSupported(void) {
    return CONFIG_LCD_ROTATION == 0 || CONFIG_LCD_ROTATION == 180;
}
static int dispW(void) { return CONFIG_LCD_NATIVE_WIDTH;  }
static int dispH(void) { return CONFIG_LCD_NATIVE_HEIGHT; }

/* Backlight: identical to the SPI panel's, because it is not part of either
 * transport — it is one LEDC channel on one pin. RC_FAST so the PWM keeps
 * toggling through light sleep; the channel is configured exactly once, since
 * re-running channel_config re-reserves the GPIO and logs a conflict each
 * time. */
static void backlightInit(void) {
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
    ledc_channel_config(&c);
    s_hasBacklight = true;
}

void lcdPanelBacklight(uint8_t level) {
    if (!s_hasBacklight) return;
    /* 255, not 2^8: the full-scale overflow value is a latch rather than a duty
     * register and is not retained across a KEEP_ALIVE light sleep. */
    ledc_set_duty(BL_MODE, BL_CH, level);
    ledc_update_duty(BL_MODE, BL_CH);
}

/* The inactivity blank. On an RGB panel "display off" stops the refresh: the
 * glass holds nothing, so what the operator sees is decided by the backlight,
 * which lcd.cpp drives to 0 alongside this. Stopping the scan is still worth
 * doing — it is the LCD DMA reading 600 KB of PSRAM per frame, forever, for a
 * screen nobody is looking at. */
void lcdPanelDisplayPower(bool on) {
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, on);
}

esp_lcd_panel_handle_t lcdPanelInit(esp_lcd_panel_io_handle_t* ioOut, int* wOut, int* hOut) {
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
    cfg.timings.h_res             = dispW();
    cfg.timings.v_res             = dispH();
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
    cfg.bounce_buffer_size_px = (size_t)CONFIG_LCD_RGB_BOUNCE_LINES * dispW();

    if (esp_lcd_new_rgb_panel(&cfg, &s_panel) != ESP_OK) {
        err("rgb panel init failed\n");
        return nullptr;
    }
    if (esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel)  != ESP_OK) {
        err("rgb panel bring-up failed\n");
        return nullptr;
    }

    if (!rotationSupported())
        warn("rotation %d is not available on an RGB panel — showing it native\n",
             CONFIG_LCD_ROTATION);
    /* 180° is both mirrors; the Kconfig toggles XOR on top of that for a glass
     * whose scan direction differs from its data sheet. */
    bool mx = (CONFIG_LCD_ROTATION == 180), my = mx;
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

    info("rgb panel %dx%d @ %d MHz\n", dispW(), dispH(), CONFIG_LCD_PCLK_MHZ);

    /* No panel-io: an RGB panel has no command channel to hand back, and
     * lcd_lvgl.cpp's colour-transfer-done callback belongs to the SPI path. */
    if (ioOut) *ioOut = nullptr;
    if (wOut)  *wOut  = dispW();
    if (hOut)  *hOut  = dispH();
    return s_panel;
}

/* Raw native (touch-chip-frame) coordinates → display coordinates. With no
 * rotation in play this is the mirrors and the clamp. */
void lcdPanelOrientTouch(int rawX, int rawY, int* outX, int* outY) {
    const int W = dispW(), H = dispH();
    int x = rawX, y = rawY;
    if (CONFIG_LCD_ROTATION == 180) { x = (W - 1) - x; y = (H - 1) - y; }
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
