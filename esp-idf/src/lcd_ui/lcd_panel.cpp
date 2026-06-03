/**
 * lcd_panel.cpp — generic SPI panel bring-up from Kconfig (CONFIG_LCD_*).
 *
 * Owns the display a board used to wire by hand: the shared SPI bus, the esp_lcd
 * panel-io, the controller (ST7789 / ILI9341), the LEDC backlight, and the
 * orientation. A board with a standard SPI panel sets the CONFIG_LCD_* pins in
 * its sdkconfig.defaults and contributes no display code. The same rotation
 * applied to the pixels here is exported (lcdPanelOrientTouch) so a board's raw
 * touch coordinates land on the right pixel.
 */
#include "lcd_internal.h"

#include "log.h"
#include "spi_helper.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#if CONFIG_LCD_CONTROLLER_ILI9341
#include "esp_lcd_ili9341.h"
#endif

#include <cstdint>

#define BL_MODE   LEDC_LOW_SPEED_MODE
#define BL_TIMER  LEDC_TIMER_0
#define BL_CH     LEDC_CHANNEL_0

static esp_lcd_panel_handle_t s_panel        = nullptr;
static bool                   s_hasBacklight = false;

/* Final (post-rotation) display size. 90°/270° swap the native axes. */
static bool rotated(void) { return CONFIG_LCD_ROTATION == 90 || CONFIG_LCD_ROTATION == 270; }
static int  dispW(void)   { return rotated() ? CONFIG_LCD_NATIVE_HEIGHT : CONFIG_LCD_NATIVE_WIDTH;  }
static int  dispH(void)   { return rotated() ? CONFIG_LCD_NATIVE_WIDTH  : CONFIG_LCD_NATIVE_HEIGHT; }

static void backlightInit(void) {
    if (CONFIG_LCD_BL_PIN < 0) return;
    ledc_timer_config_t t = {};
    t.speed_mode      = BL_MODE;
    t.duty_resolution = LEDC_TIMER_8_BIT;       /* 0..255 */
    t.timer_num       = BL_TIMER;
    t.freq_hz         = 5000;
    /* RC_FAST (not APB) so the PWM keeps toggling through light sleep; only duty
     * matters here, so RC_FAST's imprecision is fine. */
    t.clk_cfg         = LEDC_USE_RC_FAST_CLK;
    ledc_timer_config(&t);

    /* Configure the channel exactly once — this reserves the GPIO. Later changes
     * are ledc_set_duty only (lcdPanelBacklight); re-running channel_config
     * re-reserves the pin and logs a conflict on every call. */
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

/* Brightness change only — the channel is already configured (backlightInit), so
 * this is a plain duty update. KEEP_ALIVE (clocked from RC_FAST) keeps a dimmed
 * screen dimmed across light sleep rather than freezing at a random duty phase. */
void lcdPanelBacklight(uint8_t level) {
    if (!s_hasBacklight) return;
    uint32_t duty = (level == 255) ? (1u << 8) : level;   /* 8-bit: 256 = true 100% */
    ledc_set_duty(BL_MODE, BL_CH, duty);
    ledc_update_duty(BL_MODE, BL_CH);
}

/* Panel display on/off for the inactivity blank. disp_off retains GRAM, so the
 * wake is instant. */
void lcdPanelDisplayPower(bool on) {
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, on);
}

esp_lcd_panel_handle_t lcdPanelInit(esp_lcd_panel_io_handle_t* ioOut, int* wOut, int* hOut) {
    /* CONFIG_LCD_SPI_HOST is the peripheral *name* (2=SPI2/FSPI); the IDF
     * spi_host_device_t enum is offset by one (SPI2_HOST=1). Subtract — a raw
     * cast puts the panel on the wrong host where it fights the shared SD/LoRa
     * bus (blank panel, SD/LoRa failures). Mirrors tr-lora + fs.cpp. */
    const spi_host_device_t host = (spi_host_device_t)(CONFIG_LCD_SPI_HOST - 1);

    /* Shared SPI bus (idempotent — SD/LoRa may already have brought it up). */
    spi_bus_config_t bus = {};
    bus.sclk_io_num     = CONFIG_LCD_SCK_PIN;
    bus.mosi_io_num     = CONFIG_LCD_MOSI_PIN;
    bus.miso_io_num     = CONFIG_LCD_MISO_PIN;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = 4096;
    spiHelperInitBus(host, &bus);

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num       = CONFIG_LCD_CS_PIN;
    io_cfg.dc_gpio_num       = CONFIG_LCD_DC_PIN;
    io_cfg.pclk_hz           = (unsigned)CONFIG_LCD_PCLK_MHZ * 1000 * 1000;
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    io_cfg.spi_mode          = 0;
    io_cfg.trans_queue_depth = 10;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)(intptr_t)host,
                                 &io_cfg, &io) != ESP_OK) {
        err("panel-io init failed\n");
        return nullptr;
    }

    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num = CONFIG_LCD_RST_PIN;     /* -1 = resets with the power rail */
    pcfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    pcfg.bits_per_pixel = 16;
    esp_err_t perr;
#if CONFIG_LCD_CONTROLLER_ILI9341
    perr = esp_lcd_new_panel_ili9341(io, &pcfg, &s_panel);
#else
    perr = esp_lcd_new_panel_st7789(io, &pcfg, &s_panel);
#endif
    if (perr != ESP_OK) {
        err("panel controller init failed: %s\n", esp_err_to_name(perr));
        return nullptr;
    }

    /* Hardware orientation: rotate via swap_xy + mirror so LVGL renders upright
     * with no software rotation. Base swap/mirror per the standard ST7789/ILI9341
     * rotation table; the Kconfig mirror toggles XOR on top for glass whose scan
     * direction differs. */
    bool swap, mx, my;
    switch (CONFIG_LCD_ROTATION) {
        case 90:  swap = true;  mx = true;  my = false; break;
        case 180: swap = false; mx = true;  my = true;  break;
        case 270: swap = true;  mx = false; my = true;  break;
        default:  swap = false; mx = false; my = false; break;   /* 0 */
    }
#if CONFIG_LCD_MIRROR_X
    mx = !mx;
#endif
#if CONFIG_LCD_MIRROR_Y
    my = !my;
#endif

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
#if CONFIG_LCD_INVERT_COLOR
    esp_lcd_panel_invert_color(s_panel, true);
#endif
    esp_lcd_panel_swap_xy(s_panel, swap);
    esp_lcd_panel_mirror(s_panel, mx, my);
    esp_lcd_panel_disp_on_off(s_panel, true);

    backlightInit();

    /* Shared GPIO ISR service for the board's input INT lines (touch / button /
     * trackball), installed here so it exists before the board's input init runs.
     * LoRa's DIO1 path also installs it with ESP_INTR_FLAG_IRAM — match the flag
     * so whichever runs first wins; the loser gets ESP_ERR_INVALID_STATE. */
    esp_err_t isr = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE)
        warn("gpio isr service: %s\n", esp_err_to_name(isr));

    if (ioOut) *ioOut = io;
    if (wOut)  *wOut  = dispW();
    if (hOut)  *hOut  = dispH();
    return s_panel;
}

/* Raw native (touch-chip-frame) coordinates → display coordinates, matching the
 * panel's rotation + mirror above, then clamped. Verified against the T-Deck's
 * hand-rolled R=90 map (dx = ry, dy = NW-1-rx). */
void lcdPanelOrientTouch(int rawX, int rawY, int* outX, int* outY) {
    const int NW = CONFIG_LCD_NATIVE_WIDTH, NH = CONFIG_LCD_NATIVE_HEIGHT;
    int x, y;
    switch (CONFIG_LCD_ROTATION) {
        case 90:  x = rawY;            y = (NW - 1) - rawX; break;
        case 180: x = (NW - 1) - rawX; y = (NH - 1) - rawY; break;
        case 270: x = (NH - 1) - rawY; y = rawX;            break;
        default:  x = rawX;            y = rawY;            break;   /* 0 */
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
