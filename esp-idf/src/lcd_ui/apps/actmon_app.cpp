/**
 * actmon_app.cpp — "Activity": a two-tab monitor painted by hand into one
 * RGB565 canvas.
 *
 *   CPU tab   core 0 / core 1 busy % (half-height white-bar graphs) + a
 *             full-height power-mgmt graph (stacked red CPU_MAX / orange APB_MAX
 *             / yellow APB_MIN; sleep is the unfilled remainder). A 5-min average
 *             line sits under it; the current estimate floats bottom-left.
 *   wifi tab  traffic (bytes) and packets graphs — out (blue) over in (yellow),
 *             green where they overlap — auto-scaled to the window peak, with the
 *             peak value floating next to it.
 *
 * Backdrop is four subtle greyscale quarter-bands (a sawtooth gradient) instead
 * of gridlines. History for the CPU graphs comes from pmStatsHistory(), for the
 * wifi graphs from netTrafficHistory(); both rings the sampler keeps filled.
 */
#include "lcd_app.h"
#include "shell_internal.h"

#include "pm.h"
#include "net.h"
#include "storage.h"

#include <esp_heap_caps.h>
#include <cstring>
#include <cstdio>

namespace {

constexpr int TABH  = 18;                 /* tab bar height */
constexpr int CH    = 11;                 /* caption strip height */

/* CPU-tab band rects (x spans full width). */
constexpr int C0_Y = TABH + 2,  C0_H = 22;
constexpr int C1_Y = C0_Y + C0_H + CH,  C1_H = 22;
constexpr int PW_Y = C1_Y + C1_H + CH,  PW_H = 44;
constexpr int LEGEND_Y = PW_Y + PW_H;
constexpr int AVG_Y    = LEGEND_Y + CH;

/* wifi-tab band rects — packets on top, traffic on the bottom. */
constexpr int PK_Y = TABH + 12, PK_H = 46;
constexpr int TR_Y = PK_Y + PK_H + 14, TR_H = 46;

uint16_t C_WHITE, C_RED, C_ORANGE, C_YELLOW, C_BLUE, C_IN, C_MIX, C_BLACK;
bool s_colorsReady = false;

void initColors() {
    if (s_colorsReady) return;
    C_BLACK  = lv_color_to_u16(lv_color_black());
    C_WHITE  = lv_color_to_u16(lv_color_white());
    C_RED    = lv_color_to_u16(lv_color_hex(0xE05050));    /* CPU_MAX  (240 MHz) */
    C_ORANGE = lv_color_to_u16(lv_color_hex(0xF08820));    /* APB_MAX  (80 MHz, boost held) */
    C_YELLOW = lv_color_to_u16(lv_color_hex(0xE8D040));    /* APB_MIN  (80 MHz) */
    C_BLUE   = lv_color_to_u16(lv_color_hex(0x4088E8));    /* traffic out */
    C_IN     = lv_color_to_u16(lv_color_hex(0xE8D040));    /* traffic in */
    C_MIX    = lv_color_to_u16(lv_color_hex(0x46C05A));    /* in+out overlap */
    s_colorsReady = true;
}

struct State {
    lv_obj_t* canvas = nullptr;
    lv_obj_t* tabCpu = nullptr;
    lv_obj_t* tabWifi = nullptr;
    lv_obj_t* capCore0 = nullptr;
    lv_obj_t* capCore1 = nullptr;
    lv_obj_t* capLegend = nullptr;
    lv_obj_t* capAvg = nullptr;      /* 5-min avg line, no mA */
    lv_obj_t* maFloat = nullptr;     /* CPU/PM "~x mA", bottom-left of power graph */
    lv_obj_t* wifiFloat = nullptr;   /* Wi-Fi "~x mA", bottom-left of traffic graph */
    lv_obj_t* inoutLegend = nullptr; /* "IN / OUT" under the traffic graph */
    lv_obj_t* trafPeak = nullptr;
    lv_obj_t* pktPeak = nullptr;
    uint16_t* buf = nullptr;
    int W = 0, H = 0, stridePx = 0;
    PmStatSample*  hist = nullptr;   /* W entries, newest last */
    NetTrafSample* traf = nullptr;   /* W entries, newest last */
    int tab = 0;                     /* 0 = CPU, 1 = wifi */
    bool visible = false;
};
State s;

inline void px(int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)s.W && (unsigned)y < (unsigned)s.H)
        s.buf[y * s.stridePx + x] = c;
}
inline void vseg(int x, int yTop, int yBot, uint16_t c) {
    for (int y = yTop; y <= yBot; y++) px(x, y, c);
}

/* p% (0..100) -> filled pixels within an h-tall band, rounded. */
inline int pctPx(int p, int h) {
    int v = (p * (h - 1) + 50) / 100;
    if (v > h - 1) v = h - 1;
    if (v < 0) v = 0;
    return v;
}

/* value/peak (0..1 as num/den) -> filled pixels within an h-tall band. */
inline int scalePx(uint32_t num, uint32_t den, int h) {
    if (den == 0) return 0;
    long v = ((long)num * (h - 1) + den / 2) / den;
    if (v > h - 1) v = h - 1;
    if (v < 0) v = 0;
    return (int)v;
}

/* Four greyscale quarter-bands with a sawtooth ramp (dark at each quarter's
 * bottom, light at its top) — the subtle stand-in for gridlines. */
void drawBands(int y0, int h) {
    int qh = h / 4; if (qh < 1) qh = 1;
    for (int r = 0; r < h; r++) {
        int inq = (r % qh);
        int lvl = 0x31 - (inq * (0x31 - 0x24)) / qh;   /* light(0x31)→dark(0x24) top→bottom of a quarter */
        uint16_t g = lv_color_to_u16(lv_color_make(lvl, lvl, lvl));
        int y = y0 + r;
        if ((unsigned)y >= (unsigned)s.H) break;
        uint16_t* row = &s.buf[y * s.stridePx];
        for (int x = 0; x < s.W; x++) row[x] = g;
    }
}

void drawCore(int y0, int h, int n, bool core1) {
    int bottom = y0 + h - 1;
    for (int i = 0; i < n; i++) {
        int p = core1 ? s.hist[i].core1 : s.hist[i].core0;
        int hp = pctPx(p, h);
        if (p > 0 && hp < 1) hp = 1;
        if (hp > 0) vseg(s.W - n + i, bottom - hp + 1, bottom, C_WHITE);
    }
}

void drawState(int y0, int h, int n) {
    int bottom = y0 + h - 1;
    for (int i = 0; i < n; i++) {
        int cpu = s.hist[i].cpuMax, apb = s.hist[i].apbMax, slp = s.hist[i].sleep;
        int apbMin = 100 - slp - apb - cpu; if (apbMin < 0) apbMin = 0;
        int redPx = pctPx(cpu, h), orangePx = pctPx(cpu + apb, h), yellowPx = pctPx(cpu + apb + apbMin, h);
        int x = s.W - n + i;
        if (redPx > 0)           vseg(x, bottom - redPx + 1,    bottom,            C_RED);
        if (orangePx > redPx)    vseg(x, bottom - orangePx + 1, bottom - redPx,    C_ORANGE);
        if (yellowPx > orangePx) vseg(x, bottom - yellowPx + 1, bottom - orangePx, C_YELLOW);
    }
}

/* out (blue) over in (yellow), green overlap; auto-scaled to the window peak.
 * Positions `peakLabel` (right of the peak column, or left past halfway). */
void drawTraffic(int y0, int h, int n, bool packets, lv_obj_t* peakLabel,
                 const char* (*fmt)(uint32_t, char*, size_t)) {
    int bottom = y0 + h - 1;
    uint32_t peak = 0; int peakCol = -1;
    for (int i = 0; i < n; i++) {
        uint32_t o = packets ? s.traf[i].pktsOut : s.traf[i].bytesOut;
        uint32_t in = packets ? s.traf[i].pktsIn  : s.traf[i].bytesIn;
        uint32_t m = o > in ? o : in;
        if (m > peak) { peak = m; peakCol = i; }
    }
    if (peak == 0) { if (peakLabel) lv_obj_add_flag(peakLabel, LV_OBJ_FLAG_HIDDEN); return; }
    for (int i = 0; i < n; i++) {
        uint32_t o = packets ? s.traf[i].pktsOut : s.traf[i].bytesOut;
        uint32_t in = packets ? s.traf[i].pktsIn  : s.traf[i].bytesIn;
        int outPx = scalePx(o, peak, h), inPx = scalePx(in, peak, h);
        int lo = outPx < inPx ? outPx : inPx, hi = outPx > inPx ? outPx : inPx;
        int x = s.W - n + i;
        if (lo > 0)      vseg(x, bottom - lo + 1, bottom, C_MIX);
        if (hi > lo)     vseg(x, bottom - hi + 1, bottom - lo, o >= in ? C_YELLOW : C_BLUE);
    }
    if (peakLabel) {
        char buf[24];
        lv_label_set_text(peakLabel, fmt(peak, buf, sizeof buf));
        lv_obj_clear_flag(peakLabel, LV_OBJ_FLAG_HIDDEN);
        int px_ = s.W - n + (peakCol < 0 ? n - 1 : peakCol);
        if (px_ > s.W / 2) lv_obj_align(peakLabel, LV_ALIGN_TOP_RIGHT, -(s.W - px_) - 2, y0 + 2);
        else               lv_obj_align(peakLabel, LV_ALIGN_TOP_LEFT,  px_ + 2,          y0 + 2);
    }
}

/* "1.3 Mbps" / "456 kbps" from bytes/s. */
const char* fmtRate(uint32_t bytesPerSec, char* buf, size_t n) {
    uint64_t bps = (uint64_t)bytesPerSec * 8;
    if (bps >= 1000000ULL)      snprintf(buf, n, "%u.%u Mbps", (unsigned)(bps / 1000000ULL), (unsigned)((bps / 100000ULL) % 10));
    else if (bps >= 1000ULL)    snprintf(buf, n, "%u kbps", (unsigned)(bps / 1000ULL));
    else                        snprintf(buf, n, "%u bps", (unsigned)bps);
    return buf;
}
/* "34k pps" / "1.2k pps" from packets/s. */
const char* fmtPkts(uint32_t pps, char* buf, size_t n) {
    if (pps >= 10000)     snprintf(buf, n, "%uk pps", (unsigned)((pps + 500) / 1000));
    else if (pps >= 1000) snprintf(buf, n, "%u.%uk pps", (unsigned)(pps / 1000), (unsigned)((pps / 100) % 10));
    else                  snprintf(buf, n, "%u pps", (unsigned)pps);
    return buf;
}

void clearAll() {
    int total = s.stridePx * s.H;
    for (int i = 0; i < total; i++) s.buf[i] = C_BLACK;
}

void formatAvgLine(char* buf, size_t n, const PmStatAvg& a) {
    snprintf(buf, n, "5 min avg %d%% CPU_MAX, %d%% APB_MAX, %d%% APB_MIN",
             a.cpuMax, a.apbMax, a.apbMin);
}
/* One decimal below 10 mA, integer above. */
void formatMa10(char* buf, size_t n, int ma10) {
    if (ma10 < 100) snprintf(buf, n, "~%d.%d mA", ma10 / 10, ma10 % 10);
    else            snprintf(buf, n, "~%d mA", (ma10 + 5) / 10);
}

void showCpuLabels(bool on) {
    lv_obj_t* cpu[] = { s.capCore0, s.capCore1, s.capLegend, s.capAvg, s.maFloat };
    for (lv_obj_t* o : cpu) if (o) { if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
    lv_obj_t* wifi[] = { s.trafPeak, s.pktPeak, s.wifiFloat, s.inoutLegend };
    for (lv_obj_t* o : wifi) if (o) { if (on) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); }
}

void drawAll() {
    if (!s.canvas || !s.buf) return;
    clearAll();

    if (s.tab == 0) {
        if (!s.hist) return;
        int n = pmStatsHistory(s.hist, s.W);
        drawBands(C0_Y, C0_H); drawCore(C0_Y, C0_H, n, false);
        drawBands(C1_Y, C1_H); drawCore(C1_Y, C1_H, n, true);
        drawBands(PW_Y, PW_H); drawState(PW_Y, PW_H, n);

        PmStatAvg a; pmStatsAvg(&a, 300);
        char buf[96];
        if (s.capAvg)  { formatAvgLine(buf, sizeof buf, a); lv_label_set_text(s.capAvg, buf); }
        if (s.maFloat) { formatMa10(buf, sizeof buf, a.mA10); lv_label_set_text(s.maFloat, buf); }
    } else {
        if (!s.traf) return;
        int n = netTrafficHistory(s.traf, s.W);
        drawBands(PK_Y, PK_H); drawTraffic(PK_Y, PK_H, n, true,  s.pktPeak,  fmtPkts);
        drawBands(TR_Y, TR_H); drawTraffic(TR_Y, TR_H, n, false, s.trafPeak, fmtRate);
        if (s.wifiFloat) { char b[24]; formatMa10(b, sizeof b, netTrafficAvgMa10(300)); lv_label_set_text(s.wifiFloat, b); }
    }
    lv_obj_invalidate(s.canvas);
}

void tickCb(lv_timer_t*) { if (s.visible) drawAll(); }

lv_obj_t* mkCaption(lv_obj_t* root, int y, const char* text) {
    lv_obj_t* l = lv_label_create(root);
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, lcdFont(LcdFace::MONO, 8), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xC8C8C8), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 2, y);
    return l;
}

lv_obj_t* mkPeakLabel(lv_obj_t* root) {
    lv_obj_t* l = lv_label_create(root);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, lcdFont(LcdFace::MONO, 8), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE8E8E8), 0);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(l, 2, 0);
    lv_obj_set_style_radius(l, 2, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

void setTab(int t);

void tabEventCb(lv_event_t* e) {
    setTab((int)(intptr_t)lv_event_get_user_data(e));
}

lv_obj_t* mkTab(lv_obj_t* root, const char* label, int idx, int xPct) {
    lv_obj_t* b = lv_obj_create(root);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_PCT(50), TABH);
    lv_obj_set_pos(b, xPct, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, tabEventCb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, lcdFont(LcdFace::MONO, 8), 0);
    lv_obj_center(l);
    return b;
}

void styleTabs() {
    if (s.tabCpu)  lv_obj_set_style_bg_color(s.tabCpu,  lv_color_hex(s.tab == 0 ? 0x383838 : 0x202020), 0);
    if (s.tabWifi) lv_obj_set_style_bg_color(s.tabWifi, lv_color_hex(s.tab == 1 ? 0x383838 : 0x202020), 0);
}

void setTab(int t) {
    s.tab = t;
    styleTabs();
    showCpuLabels(t == 0);
    drawAll();
}

class ActmonApp : public LcdApp {
public:
    ActmonApp() : LcdApp({ .name = "Activity", .iconBasename = "actmon" }) {}

    void onCreate(lv_obj_t* root) override {
        initColors();

        int W = lv_obj_get_content_width(root);
        int H = lv_obj_get_content_height(root);
        if (W <= 0) W = 320;
        if (H <= 0) H = 200;

        uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)W, LV_COLOR_FORMAT_RGB565);
        s.buf  = (uint16_t*)heap_caps_malloc((size_t)stride * H, MALLOC_CAP_SPIRAM);
        s.hist = (PmStatSample*)heap_caps_malloc((size_t)W * sizeof(PmStatSample), MALLOC_CAP_SPIRAM);
        s.traf = (NetTrafSample*)heap_caps_malloc((size_t)W * sizeof(NetTrafSample), MALLOC_CAP_SPIRAM);
        if (!s.buf || !s.hist || !s.traf) {
            free(s.buf); free(s.hist); free(s.traf);
            s.buf = nullptr; s.hist = nullptr; s.traf = nullptr; return;
        }
        s.W = W; s.H = H; s.stridePx = (int)(stride / 2);

        s.canvas = lv_canvas_create(root);
        lv_canvas_set_buffer(s.canvas, s.buf, W, H, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(s.canvas, lv_color_black(), LV_OPA_COVER);
        lv_obj_align(s.canvas, LV_ALIGN_TOP_LEFT, 0, 0);

        s.tabCpu  = mkTab(root, "CPU",  0, 0);
        s.tabWifi = mkTab(root, "wifi", 1, LV_PCT(50));

        s.capCore0  = mkCaption(root, C0_Y + C0_H, "core 0");
        s.capCore1  = mkCaption(root, C1_Y + C1_H, "core 1");
        s.capLegend = mkCaption(root, LEGEND_Y, "power mgmt: #E05050 CPU_MAX#, #F08820 APB_MAX#, "
                                                "#E8D040 APB_MIN#. No bar: SLEEP");
        s.capAvg    = mkCaption(root, AVG_Y, "");

        /* Estimate floats — bottom-left, over their own graph. */
        s.maFloat = lv_label_create(root);
        lv_label_set_text(s.maFloat, "");
        lv_obj_set_style_text_font(s.maFloat, lcdFont(LcdFace::MONO, 8), 0);
        lv_obj_set_style_text_color(s.maFloat, lv_color_hex(0xE0E0E0), 0);
        lv_obj_align(s.maFloat, LV_ALIGN_TOP_LEFT, 2, PW_Y + PW_H - 11);

        s.wifiFloat = lv_label_create(root);
        lv_label_set_text(s.wifiFloat, "");
        lv_obj_set_style_text_font(s.wifiFloat, lcdFont(LcdFace::MONO, 8), 0);
        lv_obj_set_style_text_color(s.wifiFloat, lv_color_hex(0xE0E0E0), 0);
        lv_obj_align(s.wifiFloat, LV_ALIGN_TOP_LEFT, 2, TR_Y + TR_H - 11);

        /* IN / OUT legend under the traffic graph (bottom). */
        s.inoutLegend = mkCaption(root, TR_Y + TR_H, "#4088E8 IN# / #E8D040 OUT#");

        s.trafPeak = mkPeakLabel(root);
        s.pktPeak  = mkPeakLabel(root);

        s.tab = 0;
        styleTabs();
        showCpuLabels(true);
        drawAll();
        timer(tickCb, 1000, this);
    }

    void onShow() override { s.visible = true; storageSet("sys.stats.lcd_actmon", 1); drawAll(); }
    void onHide() override { s.visible = false; storageSet("sys.stats.lcd_actmon", 0); }

    void onClose() override {
        storageSet("sys.stats.lcd_actmon", 0);
        free(s.buf);
        free(s.hist);
        free(s.traf);
        s = State{};
    }
};

}  // namespace

LcdApp* lcdMakeActmonApp(void) { return new ActmonApp(); }
