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
#include "stylesheet.h"
#include "log.h"

#include "pm.h"
#include "net.h"
#include "storage.h"

#include <esp_heap_caps.h>
#include <cstring>
#include <cstdio>

namespace {

/* Every length here is a reference pixel put through the UI zoom, like the rest
 * of the shell: the canvas is real screen pixels, so a band stated as 22 is 22
 * of a 320x240 deck's and a fifth of that on a panel at 175%. */
/* The bands take the height they are GIVEN, not a height written for one deck:
 * the graphs are what this screen is for, so they divide whatever is left after
 * the tab bar and the caption strips, at any display size or zoom. A caption
 * strip is a line of the mono face plus its leading, because that is what has
 * to fit in it. */
inline int TABH() { return lcdPx(26); }   /* tab bar height */
inline int CH() {
    const lv_font_t* f = lcdFontMono();
    return (f ? (int)lv_font_get_line_height(f) : lcdPx(11)) + lcdPx(3);
}


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
    int head = 0;                    /* column holding the newest sample */
    uint32_t peakPkts = 0, peakTraf = 0;   /* the scale each traffic graph is drawn to */
};
State s;

/* CPU tab: two core bands and a taller power band, over four caption strips. */
inline int cpuAvail() { int a = s.H - TABH() - 4 * CH() - lcdPx(4); return a > 40 ? a : 40; }
inline int C0_H() { return cpuAvail() * 24 / 100; }
inline int C1_H() { return cpuAvail() * 24 / 100; }
inline int PW_H() { return cpuAvail() * 48 / 100; }
inline int C0_Y() { return TABH() + lcdPx(2); }
inline int C1_Y() { return C0_Y() + C0_H() + CH(); }
inline int PW_Y() { return C1_Y() + C1_H() + CH(); }
inline int LEGEND_Y() { return PW_Y() + PW_H(); }
inline int AVG_Y()    { return LEGEND_Y() + CH(); }

/* wifi tab: packets on top, traffic below, over two caption strips. */
inline int netAvail() { int a = s.H - TABH() - 2 * CH() - lcdPx(16); return a > 40 ? a : 40; }
inline int PK_H() { return netAvail() * 48 / 100; }
inline int TR_H() { return netAvail() * 48 / 100; }
inline int PK_Y() { return TABH() + lcdPx(12); }
inline int TR_Y() { return PK_Y() + PK_H() + CH() + lcdPx(4); }

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
void drawBands(int y0, int h, int xFrom, int xTo) {
    int qh = h / 4; if (qh < 1) qh = 1;
    for (int r = 0; r < h; r++) {
        int inq = (r % qh);
        int lvl = 0x31 - (inq * (0x31 - 0x24)) / qh;   /* light(0x31)→dark(0x24) top→bottom of a quarter */
        uint16_t g = lv_color_to_u16(lv_color_make(lvl, lvl, lvl));
        int y = y0 + r;
        if ((unsigned)y >= (unsigned)s.H) break;
        uint16_t* row = &s.buf[y * s.stridePx];
        for (int x = xFrom; x <= xTo && x < s.W; x++) row[x] = g;
    }
}

void drawCore(int y0, int h, int x, const PmStatSample& sm, bool core1) {
    int bottom = y0 + h - 1;
    int p = core1 ? sm.core1 : sm.core0;
    int hp = pctPx(p, h);
    if (p > 0 && hp < 1) hp = 1;
    if (hp > 0) vseg(x, bottom - hp + 1, bottom, C_WHITE);
}

void drawState(int y0, int h, int x, const PmStatSample& sm) {
    int bottom = y0 + h - 1;
    int cpu = sm.cpuMax, apb = sm.apbMax, slp = sm.sleep;
    int apbMin = 100 - slp - apb - cpu; if (apbMin < 0) apbMin = 0;
    int redPx = pctPx(cpu, h), orangePx = pctPx(cpu + apb, h), yellowPx = pctPx(cpu + apb + apbMin, h);
    if (redPx > 0)           vseg(x, bottom - redPx + 1,    bottom,            C_RED);
    if (orangePx > redPx)    vseg(x, bottom - orangePx + 1, bottom - redPx,    C_ORANGE);
    if (yellowPx > orangePx) vseg(x, bottom - yellowPx + 1, bottom - orangePx, C_YELLOW);
}

/* out (blue) over in (yellow), green overlap; auto-scaled to the window peak.
 * Positions `peakLabel` (right of the peak column, or left past halfway). */
void drawTraffic(int y0, int h, int x, const NetTrafSample& sm, bool packets, uint32_t peak) {
    if (peak == 0) return;
    int bottom = y0 + h - 1;
    uint32_t o  = packets ? sm.pktsOut : sm.bytesOut;
    uint32_t in = packets ? sm.pktsIn  : sm.bytesIn;
    int outPx = scalePx(o, peak, h), inPx = scalePx(in, peak, h);
    int lo = outPx < inPx ? outPx : inPx, hi = outPx > inPx ? outPx : inPx;
    if (lo > 0)  vseg(x, bottom - lo + 1, bottom, C_MIX);
    if (hi > lo) vseg(x, bottom - hi + 1, bottom - lo, o >= in ? C_YELLOW : C_BLUE);
}

uint32_t trafPeakOf(int n, bool packets) {
    uint32_t peak = 0;
    for (int i = 0; i < n; i++) {
        uint32_t o  = packets ? s.traf[i].pktsOut : s.traf[i].bytesOut;
        uint32_t in = packets ? s.traf[i].pktsIn  : s.traf[i].bytesIn;
        uint32_t m = o > in ? o : in;
        if (m > peak) peak = m;
    }
    return peak;
}

void placePeakLabel(lv_obj_t* peakLabel, int y0, uint32_t peak,
                    const char* (*fmt)(uint32_t, char*, size_t)) {
    if (!peakLabel) return;
    if (peak == 0) { lv_obj_add_flag(peakLabel, LV_OBJ_FLAG_HIDDEN); return; }
    char buf[24];
    lv_label_set_text(peakLabel, fmt(peak, buf, sizeof buf));
    lv_obj_clear_flag(peakLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(peakLabel, LV_ALIGN_TOP_RIGHT, -lcdPx(2), y0 + lcdPx(2));
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

/* A SWEEP, NOT A SCROLL, and the difference is the whole cost of this app.
 *
 * Scrolling means every column holds a different sample than it did a second
 * ago, so every pixel of the graph is new and the whole canvas has to reach the
 * glass again: on this board, a megabyte and a half through the one memory the
 * display is being refreshed from, once a second, which the panel cannot take.
 * Sweeping writes the newest sample OVER THE OLDEST, at a head that walks
 * across and wraps — an ECG rather than a ticker tape. Every other column still
 * holds exactly what it held, so one column is redrawn and one column's worth
 * of screen is sent.
 *
 * A full redraw is still needed when what the columns MEAN changes: the first
 * paint, a tab switch, and a new peak on the traffic graph, which is scaled to
 * the window. */
void drawColumns(int xFrom, int xTo, int n) {
    for (int x = xFrom; x <= xTo; x++) {
        /* Which sample belongs in this column: the head holds the newest, and
         * age increases leftwards, wrapping at the edge. */
        int age = (s.head - x + s.W) % s.W;
        int i   = n - 1 - age;
        if (s.tab == 0) {
            drawBands(C0_Y(), C0_H(), x, x);
            drawBands(C1_Y(), C1_H(), x, x);
            drawBands(PW_Y(), PW_H(), x, x);
            if (i < 0 || i >= n) continue;
            drawCore(C0_Y(), C0_H(), x, s.hist[i], false);
            drawCore(C1_Y(), C1_H(), x, s.hist[i], true);
            drawState(PW_Y(), PW_H(), x, s.hist[i]);
        } else {
            drawBands(PK_Y(), PK_H(), x, x);
            drawBands(TR_Y(), TR_H(), x, x);
            if (i < 0 || i >= n) continue;
            drawTraffic(PK_Y(), PK_H(), x, s.traf[i], true,  s.peakPkts);
            drawTraffic(TR_Y(), TR_H(), x, s.traf[i], false, s.peakTraf);
        }
    }
}

void clearColumn(int x) {
    for (int y = 0; y < s.H; y++) s.buf[y * s.stridePx + x] = C_BLACK;
}

/* The gap ahead of the head. Once the trace has wrapped, every column holds a
 * real sample and the only thing marking NOW is the discontinuity — which, a
 * column or two wide, is a thing to be searched for rather than seen. A twentieth
 * of the width is visible at a glance at any screen size, and it costs the
 * oldest samples on the screen, which are the least valuable ones there.
 *
 * It is drawn EMPTY, not black: the band gradients stay, so the gap reads as
 * graph with nothing in it yet rather than as a hole cut out of the picture. */
inline int gapCols() { int g = s.W * 5 / 100; return g > 2 ? g : 2; }

void drawGapColumn(int x) {
    clearColumn(x);
    if (s.tab == 0) {
        drawBands(C0_Y(), C0_H(), x, x);
        drawBands(C1_Y(), C1_H(), x, x);
        drawBands(PW_Y(), PW_H(), x, x);
    } else {
        drawBands(PK_Y(), PK_H(), x, x);
        drawBands(TR_Y(), TR_H(), x, x);
    }
}

void clearGapAhead(int head) {
    const int g = gapCols();
    for (int k = 1; k <= g; k++) drawGapColumn((head + k) % s.W);
}

void invalidateColumns(int xFrom, int xTo) {
    lv_area_t a;
    a.x1 = xFrom; a.y1 = 0; a.x2 = xTo; a.y2 = s.H - 1;
    lv_obj_invalidate_area(s.canvas, &a);
}

void drawAll(bool full) {
    if (!s.canvas || !s.buf) return;

    int n = 0;
    uint32_t pk = 0, tr = 0;
    if (s.tab == 0) {
        if (!s.hist) return;
        n = pmStatsHistory(s.hist, s.W);
        PmStatAvg a; pmStatsAvg(&a, 300);
        char buf[96];
        if (s.capAvg)  { formatAvgLine(buf, sizeof buf, a); lv_label_set_text(s.capAvg, buf); }
        if (s.maFloat) { formatMa10(buf, sizeof buf, a.mA10); lv_label_set_text(s.maFloat, buf); }
    } else {
        if (!s.traf) return;
        n = netTrafficHistory(s.traf, s.W);
        pk = trafPeakOf(n, true);
        tr = trafPeakOf(n, false);
        /* The scale is the window's peak, so a new one restates every column. */
        if (pk != s.peakPkts || tr != s.peakTraf) full = true;
        s.peakPkts = pk; s.peakTraf = tr;
        placePeakLabel(s.pktPeak,  PK_Y(), pk, fmtPkts);
        placePeakLabel(s.trafPeak, TR_Y(), tr, fmtRate);
        if (s.wifiFloat) { char b[24]; formatMa10(b, sizeof b, netTrafficAvgMa10(300)); lv_label_set_text(s.wifiFloat, b); }
    }

    if (full) {
        clearAll();
        /* The head goes where the history ENDS, not at the right edge: a graph
         * holding fewer samples than the screen is wide fills from the left and
         * grows rightwards, and the sweep carries straight on from there. Put
         * the newest at the right edge instead and every column left of it is
         * either empty or about to be overwritten from the far side — which
         * reads as old data stranded on the right. */
        s.head = (n > 0 ? n - 1 : 0) % s.W;
        drawColumns(0, s.W - 1, n);
        clearGapAhead(s.head);
        lv_obj_invalidate(s.canvas);
        return;
    }

    s.head = (s.head + 1) % s.W;
    drawColumns(s.head, s.head, n);
    clearGapAhead(s.head);
    int last = (s.head + gapCols()) % s.W;
    if (last >= s.head) invalidateColumns(s.head, last);
    else { invalidateColumns(s.head, s.W - 1); invalidateColumns(0, last); }
}

void tickCb(lv_timer_t*) { if (s.visible) drawAll(false); }

lv_obj_t* mkCaption(lv_obj_t* root, int y, const char* text) {
    lv_obj_t* l = lv_label_create(root);
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, lcdStyle().core.monoFont, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xC8C8C8), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, lcdPx(2), y);
    return l;
}

lv_obj_t* mkPeakLabel(lv_obj_t* root) {
    lv_obj_t* l = lv_label_create(root);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, lcdStyle().core.monoFont, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE8E8E8), 0);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(l, lcdPx(2), 0);
    lv_obj_set_style_radius(l, lcdPx(2), 0);
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
    lv_obj_set_size(b, LV_PCT(50), TABH());
    lv_obj_set_pos(b, xPct, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, tabEventCb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, label);
    /* The UI face, not the mono one: a tab is a control to be hit, not a column
     * of figures to be read. */
    lv_obj_set_style_text_font(l, lcdFont(LcdFace::UI, lcdPx(15)), 0);
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
    drawAll(true);
}

class ActmonApp : public LcdApp {
public:
    ActmonApp() : LcdApp({ .name = "Activity", .iconBasename = "actmon" }) {}

    void onCreate(lv_obj_t* root) override {
        initColors();

        /* The root is sized by the shell but not laid out until LVGL's next
         * pass, so ask for that pass now: read too early and the canvas is
         * built to a guess and stays that size for the life of the app. */
        lv_obj_update_layout(root);
        int W = lv_obj_get_content_width(root);
        int H = lv_obj_get_content_height(root);
        if (W <= 0) W = lcdStyle().displayW;
        if (H <= 0) H = lcdStyle().displayH - lcdStyle().statusBar.h;

        uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)W, LV_COLOR_FORMAT_RGB565);
        s.buf  = (uint16_t*)heap_caps_malloc((size_t)stride * H, MALLOC_CAP_SPIRAM);
        s.hist = (PmStatSample*)heap_caps_malloc((size_t)W * sizeof(PmStatSample), MALLOC_CAP_SPIRAM);
        s.traf = (NetTrafSample*)heap_caps_malloc((size_t)W * sizeof(NetTrafSample), MALLOC_CAP_SPIRAM);
        if (!s.buf || !s.hist || !s.traf) {
            /* A full-screen RGB565 canvas is half a megabyte of PSRAM, and
             * whether it can be had depends on what else is resident — so this
             * fails intermittently, and failing silently is an app that opens
             * onto nothing and gives no reason. Say so, then try again at half
             * the height, which is a graph rather than a blank screen. */
            warn("actmon: %dx%d canvas (%u B) refused; halving\n", W, H,
                 (unsigned)((size_t)stride * H));
            free(s.buf); free(s.hist); free(s.traf);
            s.buf = nullptr; s.hist = nullptr; s.traf = nullptr;
            H = H / 2;
            if (H < 64) return;
            s.buf  = (uint16_t*)heap_caps_malloc((size_t)stride * H, MALLOC_CAP_SPIRAM);
            s.hist = (PmStatSample*)heap_caps_malloc((size_t)W * sizeof(PmStatSample), MALLOC_CAP_SPIRAM);
            s.traf = (NetTrafSample*)heap_caps_malloc((size_t)W * sizeof(NetTrafSample), MALLOC_CAP_SPIRAM);
            if (!s.buf || !s.hist || !s.traf) {
                err("actmon: no canvas at %dx%d either\n", W, H);
                free(s.buf); free(s.hist); free(s.traf);
                s.buf = nullptr; s.hist = nullptr; s.traf = nullptr; return;
            }
        }
        s.W = W; s.H = H; s.stridePx = (int)(stride / 2);

        /* A column per sample, so the ring has to be at least as wide as the
         * canvas — the shipped 320 is a 320-px deck's width, and on a wider
         * screen it is a graph that can never fill. The sampler allocates its
         * rings when the first watcher arrives, which is the pmStatsWatch below,
         * so this lands in time. 640 seconds of CPU history is 3.2 KB of PSRAM
         * and the same of traffic history is 10 KB. */
        if (storageGetInt("s.sys.cpu_sample_buf", 320) < W)
            storageSet("s.sys.cpu_sample_buf", W);

        s.canvas = lv_canvas_create(root);
        lv_canvas_set_buffer(s.canvas, s.buf, W, H, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(s.canvas, lv_color_black(), LV_OPA_COVER);
        lv_obj_align(s.canvas, LV_ALIGN_TOP_LEFT, 0, 0);

        s.tabCpu  = mkTab(root, "CPU",  0, 0);
        s.tabWifi = mkTab(root, "wifi", 1, LV_PCT(50));

        s.capCore0  = mkCaption(root, C0_Y() + C0_H(), "core 0");
        s.capCore1  = mkCaption(root, C1_Y() + C1_H(), "core 1");
        s.capLegend = mkCaption(root, LEGEND_Y(), "power mgmt: #E05050 CPU_MAX#, #F08820 APB_MAX#, "
                                                  "#E8D040 APB_MIN#. No bar: SLEEP");
        s.capAvg    = mkCaption(root, AVG_Y(), "");

        /* Estimate floats — bottom-left, over their own graph. */
        s.maFloat = lv_label_create(root);
        lv_label_set_text(s.maFloat, "");
        lv_obj_set_style_text_font(s.maFloat, lcdStyle().core.monoFont, 0);
        lv_obj_set_style_text_color(s.maFloat, lv_color_hex(0xE0E0E0), 0);
        lv_obj_align(s.maFloat, LV_ALIGN_TOP_LEFT, lcdPx(2), PW_Y() + PW_H() - lcdPx(11));

        s.wifiFloat = lv_label_create(root);
        lv_label_set_text(s.wifiFloat, "");
        lv_obj_set_style_text_font(s.wifiFloat, lcdStyle().core.monoFont, 0);
        lv_obj_set_style_text_color(s.wifiFloat, lv_color_hex(0xE0E0E0), 0);
        lv_obj_align(s.wifiFloat, LV_ALIGN_TOP_LEFT, lcdPx(2), TR_Y() + TR_H() - lcdPx(11));

        /* IN / OUT legend under the traffic graph (bottom). */
        s.inoutLegend = mkCaption(root, TR_Y() + TR_H(), "#4088E8 IN# / #E8D040 OUT#");

        s.trafPeak = mkPeakLabel(root);
        s.pktPeak  = mkPeakLabel(root);

        s.tab = 0;
        styleTabs();
        showCpuLabels(true);
        drawAll(true);
        timer(tickCb, 1000, this);
    }

    void onShow() override { s.visible = true; pmStatsWatch(true); drawAll(true); }
    void onHide() override { s.visible = false; pmStatsWatch(false); }

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
