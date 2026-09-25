/**
 * lcd_panel_dsi.cpp — MIPI-DSI panel bring-up from Kconfig (CONFIG_LCD_DSI_*),
 * in video mode. One of the three halves of the CONFIG_LCD_BUS choice; exactly
 * one of lcd_panel.cpp, lcd_panel_rgb.cpp and this file compiles to anything.
 *
 * A video-mode DSI panel is, from here, an RGB panel on a different cable: the
 * SoC's DPI engine scans a PSRAM framebuffer out over the data lanes forever,
 * and the flush is a copy into that framebuffer. What differs is the controller:
 * it is configured over the same link, in command mode, before the video
 * starts — so this file sends the board's init table (lcdPanelSetInitSequence)
 * rather than leaving it to the board's onStart.
 *
 * The DPI driver implements init and draw_bitmap and nothing else — no mirror,
 * no display on/off — so every turn of the picture is done in the copy, and the
 * blank is a DCS command over the link.
 */
#include "sdkconfig.h"

#include "lcd_panel.h"

#include <cstddef>

/* The table outlives this file's #if: a board registers it whatever transport
 * its display turned out to be built with, and only the DSI one reads it. */
static const lcd_init_cmd_t* s_initCmds  = nullptr;
static size_t                s_initCount = 0;

extern "C" void lcdPanelSetInitSequence(const lcd_init_cmd_t* cmds, size_t count) {
    s_initCmds  = cmds;
    s_initCount = cmds ? count : 0;
}

#if CONFIG_LCD_BUS_DSI

#include "lcd_internal.h"

#include "cli.h"            /* the `panel` bring-up command */
#include "log.h"
#include "spi_helper.h"     /* spiHelperEnsureGpioIsr — the one shared GPIO ISR install */

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#define BL_MODE   LEDC_LOW_SPEED_MODE
#define BL_TIMER  LEDC_TIMER_0
#define BL_CH     LEDC_CHANNEL_0

static esp_lcd_dsi_bus_handle_t  s_bus          = nullptr;
static esp_lcd_panel_io_handle_t s_io           = nullptr;   /* DBI: the command channel */
static esp_lcd_panel_handle_t    s_panel        = nullptr;   /* DPI: the video stream */
static esp_ldo_channel_handle_t  s_phyLdo       = nullptr;
static bool                      s_hasBacklight = false;
static uint16_t*                 s_turn         = nullptr;   /* one strip, turned */

static void cliPanel(const char* args);

/* The turn in force, read from s.lcd.rotation at bring-up and fixed for the life
 * of the boot. Every non-zero turn is a transform in the copy, 180 included:
 * the DPI driver has no mirror to ask for. */
static int  s_rot = CONFIG_LCD_ROTATION;
static bool s_turning = false;   /* a strip to turn into exists, so the turn is done */
static bool quarterTurn(void) { return s_turning && (s_rot == 90 || s_rot == 270); }
static int panelW(void) { return CONFIG_LCD_NATIVE_WIDTH;  }
static int panelH(void) { return CONFIG_LCD_NATIVE_HEIGHT; }
static int dispW(void)  { return quarterTurn() ? panelH() : panelW(); }
static int dispH(void)  { return quarterTurn() ? panelW() : panelH(); }

/* ── backlight ───────────────────────────────────────────────────────────────
 * One LEDC channel, as on the other transports, plus the two things a DSI
 * panel's LED driver tends to bring with it: a PWM that dims as its duty rises
 * (it trims the boost converter's feedback rather than gating its enable), and
 * an enable line of its own. */
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
    ledc_set_duty(BL_MODE, BL_CH, level);
    ledc_update_duty(BL_MODE, BL_CH);
}

/* The inactivity blank. The backlight is what the operator sees go dark
 * (lcd.cpp drives it to 0 alongside this); the panel is told as well, with the
 * standard DCS pair, so the glass stops showing the scan it keeps receiving. */
void lcdPanelDisplayPower(bool on) {
    if (!s_io) return;
    esp_lcd_panel_io_tx_param(s_io, on ? 0x29 : 0x28, nullptr, 0);
}

/* The controller's reset line, if the board gave it one: low long enough to
 * register, then the controller's own settle before it will take a command. */
static void panelReset(void) {
#if CONFIG_LCD_DSI_RST_PIN >= 0
    gpio_reset_pin((gpio_num_t)CONFIG_LCD_DSI_RST_PIN);
    gpio_set_direction((gpio_num_t)CONFIG_LCD_DSI_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)CONFIG_LCD_DSI_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)CONFIG_LCD_DSI_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
}

/* The board's table, in order, over the DBI command channel. */
static void panelSendInit(void) {
    if (!s_initCmds || !s_initCount) {
        warn("dsi panel: no init sequence registered — the controller runs on its reset values\n");
        return;
    }
    for (size_t i = 0; i < s_initCount; i++) {
        const lcd_init_cmd_t& c = s_initCmds[i];
        if (esp_lcd_panel_io_tx_param(s_io, c.cmd, c.data, c.len) != ESP_OK)
            warn("dsi panel: command 0x%02X not sent\n", c.cmd);
        if (c.delay_ms) vTaskDelay(pdMS_TO_TICKS(c.delay_ms));
    }
}

esp_lcd_panel_handle_t lcdPanelInit(esp_lcd_panel_io_handle_t* ioOut, int* wOut, int* hOut) {
    s_rot = lcdRotationSetting();      /* before anything asks how big the display is */

    /* The D-PHY's supply first: without it the PHY never leaves its unpowered
     * state and the bus below times out coming up. */
#if CONFIG_LCD_DSI_PHY_LDO_CHAN >= 0
    esp_ldo_channel_config_t ldo = {};
    ldo.chan_id    = CONFIG_LCD_DSI_PHY_LDO_CHAN;
    ldo.voltage_mv = CONFIG_LCD_DSI_PHY_LDO_MV;
    if (esp_ldo_acquire_channel(&ldo, &s_phyLdo) != ESP_OK) {
        err("dsi: PHY supply (LDO %d) would not start\n", CONFIG_LCD_DSI_PHY_LDO_CHAN);
        return nullptr;
    }
#endif

    /* phy_clk_src left 0: IDF picks the PHY PLL's reference by silicon
     * revision, which a fixed choice would get wrong on one of them. */
    esp_lcd_dsi_bus_config_t bus = {};
    bus.bus_id             = 0;
    bus.num_data_lanes     = CONFIG_LCD_DSI_LANES;
    bus.lane_bit_rate_mbps = CONFIG_LCD_DSI_LANE_MBPS;
    if (esp_lcd_new_dsi_bus(&bus, &s_bus) != ESP_OK) {
        err("dsi bus init failed\n");
        return nullptr;
    }

    esp_lcd_dbi_io_config_t dbi = {};
    dbi.virtual_channel = 0;
    dbi.lcd_cmd_bits    = 8;
    dbi.lcd_param_bits  = 8;
    if (esp_lcd_new_panel_io_dbi(s_bus, &dbi, &s_io) != ESP_OK) {
        err("dsi command channel init failed\n");
        return nullptr;
    }

    panelReset();
    /* The controller's identity, for the log: a panel that answers here has
     * power, a link, and a controller out of reset — the three things a dark
     * screen could be missing. */
    uint8_t id[3] = {};
    if (esp_lcd_panel_io_rx_param(s_io, 0x04, id, sizeof id) == ESP_OK)
        info("dsi panel id %02X %02X %02X\n", id[0], id[1], id[2]);
    else
        warn("dsi panel did not answer its ID read\n");
    panelSendInit();

    esp_lcd_dpi_panel_config_t dpi = {};
    dpi.virtual_channel    = 0;
    dpi.dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi.dpi_clock_freq_mhz = CONFIG_LCD_PCLK_MHZ;
    dpi.pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565;
    dpi.num_fbs            = 1;
    dpi.video_timing.h_size            = panelW();
    dpi.video_timing.v_size            = panelH();
    dpi.video_timing.hsync_pulse_width = CONFIG_LCD_DSI_HSYNC_PULSE;
    dpi.video_timing.hsync_back_porch  = CONFIG_LCD_DSI_HSYNC_BACK;
    dpi.video_timing.hsync_front_porch = CONFIG_LCD_DSI_HSYNC_FRONT;
    dpi.video_timing.vsync_pulse_width = CONFIG_LCD_DSI_VSYNC_PULSE;
    dpi.video_timing.vsync_back_porch  = CONFIG_LCD_DSI_VSYNC_BACK;
    dpi.video_timing.vsync_front_porch = CONFIG_LCD_DSI_VSYNC_FRONT;
    if (esp_lcd_new_panel_dpi(s_bus, &dpi, &s_panel) != ESP_OK) {
        err("dsi video panel init failed\n");
        return nullptr;
    }
    if (esp_lcd_panel_init(s_panel) != ESP_OK) {
        err("dsi video stream would not start\n");
        return nullptr;
    }

    /* The turned strip: one LVGL draw buffer's worth, the most a single flush
     * carries. Without it the display runs native rather than scrambled. */
    if (s_rot != 0) {
        /* A strip is as wide as the display, which is the panel's longer side
         * when turned a quarter and its width otherwise; the longer side covers
         * both. */
        const int    longSide = panelW() > panelH() ? panelW() : panelH();
        const size_t bytes    = (size_t)longSide * CONFIG_LCD_DSI_DRAW_LINES * sizeof(uint16_t);
        s_turn    = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        s_turning = (s_turn != nullptr);
        if (!s_turning)
            err("rotation %d needs a %u B PSRAM strip it cannot have — showing it native\n",
                s_rot, (unsigned)bytes);
    }
#if CONFIG_LCD_MIRROR_X || CONFIG_LCD_MIRROR_Y
    warn("dsi panel: LCD_MIRROR_X/Y are not applied on this transport\n");
#endif

    backlightInit();

    /* Shared GPIO ISR service for the board's input INT lines, installed here so
     * it exists before the board's input init runs — the same contract as the
     * other transports' bring-up. */
    spiHelperEnsureGpioIsr(ESP_INTR_FLAG_IRAM);

    cliRegisterCmd("panel", cliPanel);

    info("dsi panel %dx%d, %d lane%s at %d Mbps, pclk %d MHz, display %dx%d at %d\n",
         panelW(), panelH(), CONFIG_LCD_DSI_LANES, CONFIG_LCD_DSI_LANES == 1 ? "" : "s",
         CONFIG_LCD_DSI_LANE_MBPS, CONFIG_LCD_PCLK_MHZ, dispW(), dispH(),
         s_turning ? s_rot : 0);

    /* No panel-io handed back: the flush is a framebuffer copy, and lcd_lvgl.cpp's
     * colour-transfer-done callback belongs to the SPI path. */
    if (ioOut) *ioOut = nullptr;
    if (wOut)  *wOut  = dispW();
    if (hOut)  *hOut  = dispH();
    return s_panel;
}

/* A pattern owns the glass until something is pressed: the DPI engine's own
 * generator replaces the framebuffer on the wire, and LVGL's flushes are
 * dropped so that taking it down shows the UI rather than a half-painted one. */
static bool s_patternUp = false;

bool lcdPanelPatternUp(void) { return s_patternUp; }

void lcdPanelPatternClear(void) {
    if (!s_patternUp) return;
    s_patternUp = false;
    esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_NONE);
    lcdRun(ON_LCD { lv_obj_invalidate(lv_screen_active()); });
}

/* One rendered strip into the framebuffer, turned on the way when the display
 * is held at an angle. `area` is in display coordinates, `px` LVGL's buffer.
 *
 * Native, this is one draw_bitmap: the DPI driver copies the strip into the
 * framebuffer and writes the cache back over it, which is what the DMA reading
 * PSRAM needs. Turned, the strip is first laid out in s_turn as the panel reads
 * it — a transpose for a quarter turn, walked in 16x16 tiles so every cache line
 * pulled in is read sixteen times before it leaves; a straight reversal for a
 * half turn — and the same draw_bitmap takes it from there. */
void lcdPanelBlit(const lv_area_t* area, const void* px) {
    if (s_patternUp) return;
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    if (!s_turning) {
        esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                                  area->x2 + 1, area->y2 + 1, px);
        return;
    }
    const uint16_t* src = (const uint16_t*)px;
    const int PW = panelW(), PH = panelH();

    if (s_rot == 180) {
        /* Display (x,y) → panel (PW-1-x, PH-1-y): the strip reversed end to end. */
        const size_t n = (size_t)w * h;
        for (size_t k = 0; k < n; k++) s_turn[n - 1 - k] = src[k];
        const int x1 = (PW - 1) - area->x2;
        const int y1 = (PH - 1) - area->y2;
        esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x1 + w, y1 + h, s_turn);
        return;
    }

    /* Display (x,y) → panel (px,py):
     *    90°:  px = (PW-1) - y,  py = x       270°:  px = y,  py = (PH-1) - x
     * so a w x h strip becomes an h x w one. */
    const bool cw = (s_rot == 90);
    const int  TILE = 16;
    for (int j0 = 0; j0 < h; j0 += TILE) {
        const int jn = (j0 + TILE < h) ? j0 + TILE : h;
        for (int i0 = 0; i0 < w; i0 += TILE) {
            const int in = (i0 + TILE < w) ? i0 + TILE : w;
            for (int j = j0; j < jn; j++) {
                const uint16_t* s = src + (size_t)j * w + i0;
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

/* `panel` — the link's settings, and the DPI engine's own test patterns, which
 * answer the first question a dark or scrambled DSI screen raises: is the
 * link carrying a picture at all, independently of anything LVGL drew. */
static void cliPanel(const char* args) {
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s DSI panel: link settings, test bars\n",
                  CLI_HELP_COL, "panel [bars|hbars|restore|off|on]");
        if (args && args[0] == '-') {
            cliPrintf("%-*s vertical colour bars from the DPI engine itself\n",
                      CLI_HELP_COL, "panel bars");
            cliPrintf("%-*s horizontal colour bars, the same way\n",
                      CLI_HELP_COL, "panel hbars");
            cliPrintf("%-*s hand the screen back to the UI\n",
                      CLI_HELP_COL, "panel restore");
            cliPrintf("%-*s the DCS display-off / display-on pair\n",
                      CLI_HELP_COL, "panel off|on");
        }
        return;
    }
    if (args && (!strncmp(args, "bars", 4) || !strncmp(args, "hbars", 5))) {
        const bool horiz = (args[0] == 'h');
        s_patternUp = true;
        esp_lcd_dpi_panel_set_pattern(s_panel, horiz ? MIPI_DSI_PATTERN_BAR_HORIZONTAL
                                                     : MIPI_DSI_PATTERN_BAR_VERTICAL);
        cliPrintf("%s colour bars from the DPI engine. Touch the screen to put the UI back.\n",
                  horiz ? "Horizontal" : "Vertical");
        return;
    }
    if (args && !strncmp(args, "restore", 7)) {
        lcdPanelPatternClear();
        cliPrintf("repainting\n");
        return;
    }
    if (args && (!strcmp(args, "off") || !strcmp(args, "on"))) {
        lcdPanelDisplayPower(args[1] == 'n');
        cliPrintf("display %s\n", args[1] == 'n' ? "on" : "off");
        return;
    }
    cliPrintf("glass %dx%d, display %dx%d turned %d deg (s.lcd.rotation)\n",
              panelW(), panelH(), dispW(), dispH(), s_turning ? s_rot : 0);
    cliPrintf("%d lane%s at %d Mbps, pclk %d MHz, h %d/%d/%d, v %d/%d/%d\n",
              CONFIG_LCD_DSI_LANES, CONFIG_LCD_DSI_LANES == 1 ? "" : "s",
              CONFIG_LCD_DSI_LANE_MBPS, CONFIG_LCD_PCLK_MHZ,
              CONFIG_LCD_DSI_HSYNC_PULSE, CONFIG_LCD_DSI_HSYNC_BACK, CONFIG_LCD_DSI_HSYNC_FRONT,
              CONFIG_LCD_DSI_VSYNC_PULSE, CONFIG_LCD_DSI_VSYNC_BACK, CONFIG_LCD_DSI_VSYNC_FRONT);
    cliPrintf("draw strip %d lines, init sequence %u commands\n",
              CONFIG_LCD_DSI_DRAW_LINES, (unsigned)s_initCount);
}

/* Raw native (touch-chip-frame) coordinates → display coordinates: the turn the
 * pixels took, then the clamp. The same table as the RGB transport's. */
void lcdPanelOrientTouch(int rawX, int rawY, int* outX, int* outY) {
    const int NW = panelW(), NH = panelH();
    int x = rawX, y = rawY;
    if (quarterTurn()) {
        if (s_rot == 90) { x = rawY;            y = (NW - 1) - rawX; }
        else             { x = (NH - 1) - rawY; y = rawX;            }
    } else if (s_turning && s_rot == 180) {
        x = (NW - 1) - rawX; y = (NH - 1) - rawY;
    }
    const int W = dispW(), H = dispH();
    if (x < 0) x = 0; else if (x >= W) x = W - 1;
    if (y < 0) y = 0; else if (y >= H) y = H - 1;
    if (outX) *outX = x;
    if (outY) *outY = y;
}

#endif  /* CONFIG_LCD_BUS_DSI */
