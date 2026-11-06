/**
 * lcd_panel_nv3031b.cpp — esp_lcd panel driver for the NV3031B, a quad-SPI
 * panel controller with no esp_lcd driver of its own.
 *
 * Every transaction opens with a 32-bit header sent on one line:
 *
 *   02 00 <cmd> 00   then parameters, one line     a command
 *   32 00 2C 00      then pixels, four lines       a memory write
 *
 * which is what esp_lcd's SPI panel-io sends in quad mode when it is created
 * with 32-bit commands (lcd_panel.cpp): tx_param stays on one line throughout,
 * tx_color sends its header on one line and its data on four.
 *
 * The init table is the panel vendor's, as LovyanGFX's Panel_NV3031B carries
 * it. This controller scans with both axes reversed against the usual MADCTL
 * table, so the mirror bits are set against that base — which lets
 * lcd_panel.cpp's generic swap/mirror table land upright here too.
 */
#include "sdkconfig.h"
#if CONFIG_LCD_BUS_SPI && CONFIG_LCD_CONTROLLER_NV3031B

#include "lcd_internal.h"

#include "driver/gpio.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstdlib>

namespace {

constexpr int     OP_CMD    = 0x02;
constexpr int     OP_PIXELS = 0x32;
constexpr uint8_t CMD_NOP = 0x00, CMD_SWRESET = 0x01, CMD_SLPOUT = 0x11,
                  CMD_INVOFF = 0x20, CMD_INVON = 0x21, CMD_DISPOFF = 0x28,
                  CMD_DISPON = 0x29, CMD_CASET = 0x2A, CMD_RASET = 0x2B,
                  CMD_RAMWR = 0x2C, CMD_MADCTL = 0x36;
constexpr uint8_t MADCTL_MY = 0x80, MADCTL_MX = 0x40, MADCTL_MV = 0x20,
                  MADCTL_BGR = 0x08;

int header(int op, uint8_t cmd) { return (op << 24) | (cmd << 8); }

struct InitCmd {
    uint8_t  cmd;
    uint8_t  len;
    uint8_t  data[8];
    uint16_t delayMs;
};

const InitCmd kInit[] = {
    { 0xFD, 2, { 0x06, 0x08 }, 0 },                                     /* unlock factory registers */
    { 0x60, 1, { 0x0C }, 0 },                                           /* source timing */
    { 0x61, 2, { 0x07, 0x04 }, 0 },                                     /* gate timing */
    { 0xB4, 1, { 0x01 }, 0 },                                           /* 1-dot column inversion */
    { 0xB1, 3, { 0x0F, 0x02, 0x03 }, 0 },                               /* frame rate */
    { 0xB5, 4, { 0x02, 0x02, 0x0A, 0x14 }, 0 },                         /* porches */
    { 0xB6, 5, { 0x44, 0x01, 0x9F, 0x00, 0x02 }, 0 },                   /* display function */
    { 0xDF, 1, { 0x11 }, 0 },                                           /* bias/oscillator trim */
    { 0x67, 1, { 0x21 }, 0 },                                           /* bias current */
    { 0x68, 4, { 0x90, 0x4F, 0x27, 0x21 }, 0 },                         /* VCOM / power */
    { 0xE1, 2, { 0x20, 0x69 }, 0 },                                     /* gamma: positive voltage */
    { 0xE4, 2, { 0x69, 0x20 }, 0 },                                     /* gamma: negative voltage */
    { 0xE2, 6, { 0x10, 0x12, 0x12, 0x30, 0x39, 0x3F }, 0 },             /* gamma: positive mids */
    { 0xE5, 6, { 0x3F, 0x33, 0x2D, 0x12, 0x12, 0x10 }, 0 },             /* gamma: negative mids */
    { 0xE0, 8, { 0x06, 0x06, 0x0B, 0x12, 0x11, 0x11, 0x0E, 0x19 }, 0 }, /* gamma: positive curve */
    { 0xE3, 8, { 0x19, 0x13, 0x14, 0x14, 0x14, 0x12, 0x08, 0x05 }, 0 }, /* gamma: negative curve */
    { 0xE6, 2, { 0x00, 0xFF }, 0 },                                     /* AVDD/AVCL slope */
    { 0xE7, 6, { 0x01, 0x04, 0x03, 0x03, 0x00, 0x12 }, 0 },             /* source output / EQ */
    { 0xE8, 3, { 0x00, 0x70, 0x00 }, 0 },                               /* source output level */
    { 0xEC, 1, { 0x54 }, 0 },                                           /* gate EQ / charge pump */
    { 0xFD, 2, { 0xFA, 0xFC }, 0 },                                     /* lock factory registers */
    { 0x3A, 1, { 0x55 }, 0 },                                           /* COLMOD: RGB565 */
    { CMD_SLPOUT, 0, {}, 100 },
};

struct Nv3031b {
    esp_lcd_panel_t           base;
    esp_lcd_panel_io_handle_t io;
    int                       rst;
    int                       xGap, yGap;
    bool                      swap, mx, my;
    uint8_t                   bgr;
};

Nv3031b* self(esp_lcd_panel_t* p) { return __containerof(p, Nv3031b, base); }

esp_err_t send(Nv3031b* n, uint8_t cmd, const void* data, size_t len) {
    return esp_lcd_panel_io_tx_param(n->io, header(OP_CMD, cmd), data, len);
}

esp_err_t applyMadctl(Nv3031b* n) {
    uint8_t m = n->bgr;
    if (n->swap) m |= MADCTL_MV;
    if (!n->mx)  m |= MADCTL_MX;
    if (!n->my)  m |= MADCTL_MY;
    return send(n, CMD_MADCTL, &m, 1);
}

esp_err_t panelDel(esp_lcd_panel_t* p) {
    Nv3031b* n = self(p);
    if (n->rst >= 0) gpio_reset_pin((gpio_num_t)n->rst);
    free(n);
    return ESP_OK;
}

esp_err_t panelReset(esp_lcd_panel_t* p) {
    Nv3031b* n = self(p);
    if (n->rst >= 0) {
        gpio_set_level((gpio_num_t)n->rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)n->rst, 1);
    } else {
        send(n, CMD_SWRESET, nullptr, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

esp_err_t panelInit(esp_lcd_panel_t* p) {
    Nv3031b* n = self(p);
    send(n, CMD_NOP, nullptr, 0);
    for (const InitCmd& c : kInit) {
        esp_err_t e = send(n, c.cmd, c.len ? c.data : nullptr, c.len);
        if (e != ESP_OK) return e;
        if (c.delayMs) vTaskDelay(pdMS_TO_TICKS(c.delayMs));
    }
    return applyMadctl(n);
}

esp_err_t panelDraw(esp_lcd_panel_t* p, int x0, int y0, int x1, int y1, const void* px) {
    Nv3031b* n = self(p);
    const int w = x1 - x0, h = y1 - y0;
    x0 += n->xGap; x1 += n->xGap;
    y0 += n->yGap; y1 += n->yGap;
    const uint8_t ca[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0,
                            (uint8_t)((x1 - 1) >> 8), (uint8_t)(x1 - 1) };
    const uint8_t ra[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0,
                            (uint8_t)((y1 - 1) >> 8), (uint8_t)(y1 - 1) };
    send(n, CMD_CASET, ca, 4);
    send(n, CMD_RASET, ra, 4);
    send(n, CMD_RAMWR, nullptr, 0);
    return esp_lcd_panel_io_tx_color(n->io, header(OP_PIXELS, CMD_RAMWR), px,
                                     (size_t)w * h * 2);
}

esp_err_t panelInvert(esp_lcd_panel_t* p, bool invert) {
    return send(self(p), invert ? CMD_INVON : CMD_INVOFF, nullptr, 0);
}

esp_err_t panelMirror(esp_lcd_panel_t* p, bool mx, bool my) {
    Nv3031b* n = self(p);
    n->mx = mx;
    n->my = my;
    return applyMadctl(n);
}

esp_err_t panelSwap(esp_lcd_panel_t* p, bool swap) {
    Nv3031b* n = self(p);
    n->swap = swap;
    return applyMadctl(n);
}

esp_err_t panelGap(esp_lcd_panel_t* p, int x, int y) {
    Nv3031b* n = self(p);
    n->xGap = x;
    n->yGap = y;
    return ESP_OK;
}

/* DISPON takes one dummy parameter on this controller. */
esp_err_t panelOnOff(esp_lcd_panel_t* p, bool on) {
    const uint8_t dummy = 0;
    return on ? send(self(p), CMD_DISPON, &dummy, 1)
              : send(self(p), CMD_DISPOFF, nullptr, 0);
}

}  // namespace

esp_err_t lcdNewPanelNv3031b(esp_lcd_panel_io_handle_t io,
                             const esp_lcd_panel_dev_config_t* cfg,
                             esp_lcd_panel_handle_t* out) {
    auto* n = static_cast<Nv3031b*>(calloc(1, sizeof(Nv3031b)));
    if (!n) return ESP_ERR_NO_MEM;
    n->io  = io;
    n->rst = cfg->reset_gpio_num;
    n->bgr = cfg->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR ? MADCTL_BGR : 0;
    if (n->rst >= 0) {
        gpio_config_t g = {};
        g.pin_bit_mask = 1ULL << n->rst;
        g.mode         = GPIO_MODE_OUTPUT;
        gpio_config(&g);
        gpio_set_level((gpio_num_t)n->rst, 1);
    }
    n->base.del          = panelDel;
    n->base.reset        = panelReset;
    n->base.init         = panelInit;
    n->base.draw_bitmap  = panelDraw;
    n->base.invert_color = panelInvert;
    n->base.mirror       = panelMirror;
    n->base.swap_xy      = panelSwap;
    n->base.set_gap      = panelGap;
    n->base.disp_on_off  = panelOnOff;
    *out = &n->base;
    return ESP_OK;
}

#endif  /* CONFIG_LCD_BUS_SPI && CONFIG_LCD_CONTROLLER_NV3031B */
