/**
 * lcd_touch.cpp — the component-owned touch controller (CONFIG_LCD_TOUCH_*).
 *
 * Standard I2C touch parts (FT5x06 family, GT911) driven through esp_lcd_touch
 * and brought up from Kconfig exactly like the panel (lcd_panel.cpp): a board
 * with one of these contributes no touch C code, only pins. The controller's
 * INT line is wired to the shared input ISR, and lcdTouchCtlRead() does the
 * I2C read on the lcd task when touchReadCb asks (event-mode; the component
 * re-polls at ~10 ms while a finger is down). Bespoke touch — a controller
 * sampled on a board task to share its bus with other board peripherals (the
 * T-Deck), or a part esp_lcd_touch has no driver for — still comes through
 * lcd_input.h's touch_read, which touchReadCb falls back to whenever no
 * controller is selected here.
 *
 * Also home to the component-level touch request key, controller or not:
 * `lcd.multi_touch` (ephemeral, no `s.`) — a consumer sets it truthy while it
 * wants multi-finger reads + gestures (maps does, around its pinch-zoom) and
 * clears it on the way out; the subscription flips lcdTouchSetMultipoint().
 *
 * The bus: the component creates the CONFIG_LCD_TOUCH_I2C_PORT master bus
 * itself, so the touch controller must be that port's only creator — a board
 * whose bus carries other chips keeps the board-HAL fallback instead.
 *
 * In standby (sys.standby truthy) reads are gated off — the INT still fires,
 * the sample is discarded — so a resting finger cannot hold the device awake;
 * waking is the board's job (its button clears the key).
 */
#include "lcd_internal.h"
#include "i2c_helper.h"     /* SPANGAP_I2C_PULLUP (shared bus wiring policy) */
#include "log.h"
#include "storage.h"

#include "sdkconfig.h"

#include <cstdio>
#include <cstdlib>

#if CONFIG_LCD_TOUCH_CONTROLLER_FT5X06 || CONFIG_LCD_TOUCH_CONTROLLER_GT911
#define LCD_TOUCH_CTL 1
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#if CONFIG_LCD_TOUCH_CONTROLLER_FT5X06
#include "esp_lcd_touch_ft5x06.h"
#else
#include "esp_lcd_touch_gt911.h"
#endif
#else
#define LCD_TOUCH_CTL 0
#endif

#if LCD_TOUCH_CTL

static esp_lcd_touch_handle_t s_tp = nullptr;   /* null until the probe succeeds */
static bool                   s_asleep = false; /* sys.standby: reads gated */

static void touchCtlBringup(void) {
    i2c_master_bus_config_t bcfg = {};
    bcfg.i2c_port                     = CONFIG_LCD_TOUCH_I2C_PORT;
    bcfg.sda_io_num                   = (gpio_num_t)CONFIG_LCD_TOUCH_I2C_SDA;
    bcfg.scl_io_num                   = (gpio_num_t)CONFIG_LCD_TOUCH_I2C_SCL;
    bcfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bcfg.glitch_ignore_cnt            = 7;
    bcfg.flags.enable_internal_pullup = SPANGAP_I2C_PULLUP;
    i2c_master_bus_handle_t i2c = nullptr;
    if (i2c_new_master_bus(&bcfg, &i2c) != ESP_OK) {
        warn("touch: i2c bus init failed\n");
        return;
    }

    esp_lcd_touch_config_t tcfg = {};
    /* esp_lcd_touch stays at IDENTITY (native coords); touchReadCb orients the
     * points with the same CONFIG_LCD_ROTATION it applies to the pixels — so
     * the maxes are the native ranges. */
    tcfg.x_max         = CONFIG_LCD_NATIVE_WIDTH;
    tcfg.y_max         = CONFIG_LCD_NATIVE_HEIGHT;
    tcfg.rst_gpio_num  = (gpio_num_t)CONFIG_LCD_TOUCH_RST_PIN;   /* -1 = none */
    tcfg.int_gpio_num  = (gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN;
    tcfg.flags.swap_xy  = 0;
    tcfg.flags.mirror_x = 0;
    tcfg.flags.mirror_y = 0;

#if CONFIG_LCD_TOUCH_CONTROLLER_FT5X06
    const char*   name    = "FT5x06";
    /* The FT5x06 family's one fixed address. */
    const uint8_t addrs[] = { ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS };
    const int     cmdBits = 8;
#else
    const char*   name    = "GT911";
    /* The GT911 latches its address from the INT level at power-on
     * (low -> 0x5D, high -> 0x14); with no dedicated reset the level is
     * board-dependent, so probe both. */
    const uint8_t addrs[] = { ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
                              ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP };
    const int     cmdBits = 16;
#endif

    /* Driver-tag noise control: a failed read or probe logs several driver
     * lines for one event, and the one worth keeping is our own warn(). The
     * wrong-address half of a probe and a not-yet-powered controller fail this
     * way as a matter of course. */
    logRule("FT5x06: ", 'N');
    logRule("GT911: ", 'N');
    logRule("panel_io_i2c_rx_buffer", 'N');

    /* The controller runs its own firmware, usually off a gated power rail;
     * right after a cold power-on it can miss the first probe. Give the rail
     * time and retry before declaring it absent. */
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(150));
        for (uint8_t addr : addrs) {
            esp_lcd_panel_io_handle_t tio = nullptr;
            /* Built by hand: the driver CONFIG macros use out-of-order
             * designated initializers, a hard error in C++. */
            esp_lcd_panel_io_i2c_config_t io_cfg = {};
            io_cfg.dev_addr            = addr;
            io_cfg.scl_speed_hz        = CONFIG_LCD_TOUCH_I2C_KHZ * 1000;
            io_cfg.control_phase_bytes = 1;
            io_cfg.dc_bit_offset       = 0;
            io_cfg.lcd_cmd_bits        = cmdBits;
            io_cfg.flags.disable_control_phase = 1;
            if (esp_lcd_new_panel_io_i2c_v2(i2c, &io_cfg, &tio) != ESP_OK) continue;

            esp_lcd_touch_handle_t tp = nullptr;
#if CONFIG_LCD_TOUCH_CONTROLLER_FT5X06
            esp_err_t err = esp_lcd_touch_new_i2c_ft5x06(tio, &tcfg, &tp);
#else
            esp_err_t err = esp_lcd_touch_new_i2c_gt911(tio, &tcfg, &tp);
#endif
            if (err == ESP_OK) {
                info("touch: %s ready @ 0x%02X\n", name, addr);
                char tch[20];
                snprintf(tch, sizeof(tch), "%s @ 0x%02X", name, addr);
                storageSet("lcd.touch", tch);   /* for a board's Hardware pane */
                /* esp_lcd_touch configured the INT pin as input; take it
                 * interrupt-driven (ANYEDGE — polarity is part-dependent, and
                 * a redundant edge just costs one empty read). The edge wakes
                 * the lcd task, which reads the (event-mode) touch indev via
                 * lcdTouchCtlRead. */
                gpio_set_intr_type((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN, GPIO_INTR_ANYEDGE);
                gpio_isr_handler_add((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN, lcdInputISR, nullptr);
                gpio_intr_enable((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN);
                s_tp = tp;
                return;
            }
            esp_lcd_panel_io_del(tio);   /* free and try the next address / attempt */
        }
    }
    warn("touch: %s not found after 4 attempts\n", name);
    storageSet("lcd.touch", "not found");
}

#endif  /* LCD_TOUCH_CTL */

bool lcdTouchCtlConfigured(void) { return LCD_TOUCH_CTL != 0; }

/* Called from lcdLvglInit on the lcd task, after the panel (and the shared GPIO
 * ISR service) are up and after the board input HAL's own init. Storage is up
 * (lcdInit runs inside spangapInit), so the subscriptions dispatch here. */
void lcdTouchCtlInit(void) {
    /* The multi-touch request key is component-level — it drives
     * lcdTouchSetMultipoint whether touch comes from this controller or from a
     * board HAL's touch_read. */
    NOW_AND_ON_CHANGE("lcd.multi_touch", { lcdTouchSetMultipoint(atoi(val) != 0); });
#if LCD_TOUCH_CTL
    storageSubscribeChanges("sys.standby", ON_CHANGE { s_asleep = atoi(val) != 0; });
    touchCtlBringup();
#endif
}

/* touchReadCb's first source: true = this module owns touch (write the sample,
 * count 0 when no finger / asleep / controller absent), false = no controller
 * is selected and the board HAL's touch_read should be consulted instead. */
bool lcdTouchCtlRead(lcd_raw_pt_t* pts, int max, int* count) {
#if LCD_TOUCH_CTL
    *count = 0;
    if (!s_tp || s_asleep) return true;
    /* A bus glitch loses one sample and the next poll picks the finger back
     * up, so this is a warning, not an error — reported on the failing edge
     * only, since a wedged bus fails on every poll. */
    static bool readFailed = false;
    esp_err_t err = esp_lcd_touch_read_data(s_tp);
    if (err != ESP_OK) {
        if (!readFailed) warn("touch: read failed: %s\n", esp_err_to_name(err));
        readFailed = true;
        return true;
    }
    readFailed = false;
    esp_lcd_touch_point_data_t pt[LCD_TOUCH_MAXPTS] = {};
    uint8_t cnt = 0;
    esp_lcd_touch_get_data(s_tp, pt, &cnt, LCD_TOUCH_MAXPTS);
    int n = cnt > LCD_TOUCH_MAXPTS ? LCD_TOUCH_MAXPTS : cnt;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) { pts[i].x = (int16_t)pt[i].x; pts[i].y = (int16_t)pt[i].y; }
    *count = n;
    return true;
#else
    (void)pts; (void)max; (void)count;
    return false;
#endif
}
