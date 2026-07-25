/**
 * actmon_app.cpp — "Activity": three running graphs of the last screen-width
 * seconds, fed by the 1 Hz CPU/PM sampler (pm.cpp).
 *
 *   core 0        white 1 px bar/second, height = core-0 busy %
 *   core 1        white 1 px bar/second, height = core-1 busy %
 *   system state  stacked bar/second: red CPU_MAX (bottom), orange APB_MAX,
 *                 yellow APB_MIN; sleep is the unfilled remainder to the top.
 *
 * Each graph is a 320×50 band on a dark-grey field with light-grey gridlines at
 * 0/20/40/60/80/100 %, its caption left-aligned underneath. The whole body is
 * one RGB565 canvas we paint by hand each second (bars need pixel control);
 * captions are three labels over it. History comes from pmStatsHistory(), which
 * the sampler keeps filled whether or not this app is open, so opening shows the
 * last few minutes at once.
 */
#include "lcd_app.h"
#include "shell_internal.h"

#include "pm.h"

#include <esp_heap_caps.h>
#include <cstring>

namespace {

/* Band geometry, in device pixels (the ring is sized to the screen width, so a
 * sample maps to exactly one column). */
constexpr int GH    = 50;                 /* graph height */
constexpr int CH    = 12;                 /* caption strip height */
constexpr int GAP   = 2;                  /* gap below a caption */
constexpr int PITCH = GH + CH + GAP;      /* band-to-band vertical pitch */

uint16_t C_BG, C_GRID, C_WHITE, C_RED, C_ORANGE, C_YELLOW;
bool s_colorsReady = false;

void initColors() {
    if (s_colorsReady) return;
    C_BG     = lv_color_to_u16(lv_color_hex(0x2A2A2A));   /* dark grey field */
    C_GRID   = lv_color_to_u16(lv_color_hex(0x606060));   /* light grey lines */
    C_WHITE  = lv_color_to_u16(lv_color_white());
    C_RED    = lv_color_to_u16(lv_color_hex(0xE05050));    /* CPU_MAX  (240 MHz) */
    C_ORANGE = lv_color_to_u16(lv_color_hex(0xF08820));    /* APB_MAX  (80 MHz, boost held) */
    C_YELLOW = lv_color_to_u16(lv_color_hex(0xE8D040));    /* APB_MIN  (80 MHz) */
    s_colorsReady = true;
}

struct State {
    lv_obj_t*     canvas = nullptr;
    uint16_t*     buf    = nullptr;
    int           W = 0, H = 0, stridePx = 0;
    PmStatSample* hist   = nullptr;   /* scratch, W entries, newest last */
    bool          visible = false;
};
State s;

inline void px(int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)s.W && (unsigned)y < (unsigned)s.H)
        s.buf[y * s.stridePx + x] = c;
}
inline void hline(int y, uint16_t c) {                 /* contiguous row: no per-px bounds check */
    if ((unsigned)y >= (unsigned)s.H) return;
    uint16_t* row = &s.buf[y * s.stridePx];
    for (int x = 0; x < s.W; x++) row[x] = c;
}
inline void vseg(int x, int yTop, int yBot, uint16_t c) {   /* strided column */
    for (int y = yTop; y <= yBot; y++) px(x, y, c);
}

/* percent (0..100) -> filled pixels within a GH-tall band, rounded. */
inline int pctPx(int p) {
    int h = (p * (GH - 1) + 50) / 100;
    if (h > GH - 1) h = GH - 1;
    if (h < 0) h = 0;
    return h;
}
/* row of the p% gridline within band top `y0`. */
inline int gridRow(int y0, int p) { return y0 + (GH - 1) - pctPx(p); }

void drawGrid(int y0) {
    static const int levels[] = { 0, 20, 40, 60, 80, 100 };
    for (int i = 0; i < 6; i++) hline(gridRow(y0, levels[i]), C_GRID);
}

/* Core busy graph: one white bar per sample. Any nonzero % (the sampler already
 * floors nonzero busy to 1) draws at least one pixel. */
void drawCore(int band, int xBase, int n, bool core1) {
    int y0 = band * PITCH, bottom = y0 + GH - 1;
    for (int i = 0; i < n; i++) {
        int p = core1 ? s.hist[i].core1 : s.hist[i].core0;
        int h = pctPx(p);
        if (p > 0 && h < 1) h = 1;
        if (h > 0) vseg(xBase + i, bottom - h + 1, bottom, C_WHITE);
    }
}

/* System-state graph: stacked red/orange/yellow from the bottom; the sleep
 * remainder is left as background. */
void drawState(int band, int xBase, int n) {
    int y0 = band * PITCH, bottom = y0 + GH - 1;
    for (int i = 0; i < n; i++) {
        int cpu = s.hist[i].cpuMax, apb = s.hist[i].apbMax, slp = s.hist[i].sleep;
        int apbMin = 100 - slp - apb - cpu;
        if (apbMin < 0) apbMin = 0;
        int redPx    = pctPx(cpu);
        int orangePx = pctPx(cpu + apb);
        int yellowPx = pctPx(cpu + apb + apbMin);
        int x = xBase + i;
        if (redPx > 0)          vseg(x, bottom - redPx + 1,    bottom,            C_RED);
        if (orangePx > redPx)   vseg(x, bottom - orangePx + 1, bottom - redPx,    C_ORANGE);
        if (yellowPx > orangePx) vseg(x, bottom - yellowPx + 1, bottom - orangePx, C_YELLOW);
    }
}

void drawAll() {
    if (!s.canvas || !s.buf || !s.hist) return;

    /* Clear the whole buffer (padding included) to the field colour. */
    int total = s.stridePx * s.H;
    for (int i = 0; i < total; i++) s.buf[i] = C_BG;

    int n = pmStatsHistory(s.hist, s.W);
    int xBase = s.W - n;               /* right-align: newest at the right edge */

    for (int b = 0; b < 3; b++) drawGrid(b * PITCH);
    drawCore(0, xBase, n, /*core1=*/false);
    drawCore(1, xBase, n, /*core1=*/true);
    drawState(2, xBase, n);

    lv_obj_invalidate(s.canvas);
}

void tickCb(lv_timer_t*) { if (s.visible) drawAll(); }

void mkCaption(lv_obj_t* root, int band, const char* text) {
    lv_obj_t* l = lv_label_create(root);
    lv_label_set_recolor(l, true);        /* honour inline #RRGGBB spans */
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, lcdFont(LcdFace::MONO, 8), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xC8C8C8), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 2, band * PITCH + GH);
}

class ActmonApp : public LcdApp {
public:
    ActmonApp() : LcdApp({ .name = "Activity", .iconBasename = "actmon" }) {}

    void onCreate(lv_obj_t* root) override {
        initColors();

        int W = lv_obj_get_content_width(root);
        int H = lv_obj_get_content_height(root);
        if (W <= 0) W = 320;
        if (H <= 0) H = 3 * PITCH;

        uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)W, LV_COLOR_FORMAT_RGB565);
        s.buf = (uint16_t*)heap_caps_malloc((size_t)stride * H, MALLOC_CAP_SPIRAM);
        s.hist = (PmStatSample*)heap_caps_malloc((size_t)W * sizeof(PmStatSample), MALLOC_CAP_SPIRAM);
        if (!s.buf || !s.hist) { free(s.buf); free(s.hist); s.buf = nullptr; s.hist = nullptr; return; }
        s.W = W; s.H = H; s.stridePx = (int)(stride / 2);

        s.canvas = lv_canvas_create(root);
        lv_canvas_set_buffer(s.canvas, s.buf, W, H, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(s.canvas, lv_color_hex(0x2A2A2A), LV_OPA_COVER);
        lv_obj_align(s.canvas, LV_ALIGN_TOP_LEFT, 0, 0);

        mkCaption(root, 0, "core 0");
        mkCaption(root, 1, "core 1");
        mkCaption(root, 2, "power mgmt: #E05050 CPU_MAX#, #F08820 APB_MAX#, "
                           "#E8D040 APB_MIN#. Rest is SLEEP");

        drawAll();
        timer(tickCb, 1000, this);   /* ledgered: freed on close */
    }

    void onShow() override { s.visible = true; drawAll(); }
    void onHide() override { s.visible = false; }

    void onClose() override {
        /* Labels + canvas widget free with the layer tree; the backing buffers
         * are ours to release. */
        free(s.buf);
        free(s.hist);
        s = State{};
    }
};

}  // namespace

LcdApp* lcdMakeActmonApp(void) { return new ActmonApp(); }
