/**
 * stylesheet.h — the phone shell's theme/geometry as data (Brookesia's one bone
 * most worth keeping). One nested struct per (name, screen_size); at shellInit
 * the active sheet is selected by the real panel size and calibrated (percent ->
 * px). The shell reads geometry/colour/font from here instead of #define'd magic
 * numbers, so a second board is a data change (a new sheet), not a code change.
 *
 * Seeded from spangap-lcd's CURRENT pixel values so the new shell matches today.
 * Ship the 320x240 dark sheet only (stylesheet_320x240.cpp); other boards/themes
 * are later additions, as data.
 */
#pragma once

#include "lvgl.h"
#include <cstdint>

struct LcdStyle {
    const char* name;        /* "default" / a board name */
    int         displayW;    /* the panel size this sheet targets (0 = any) */
    int         displayH;

    struct Core {
        uint32_t         bg;              /* base background (launcher) */
        const lv_font_t* font;            /* default label/clock/status font */
        const lv_font_t* titleFont;       /* headings */
        int              maxResidentApps; /* evict the LRU past this many roots */
    } core;

    struct StatusBar {
        int      h;
        uint32_t bg;
        uint32_t text;
    } statusBar;

    struct Launcher {
        int              cols, rows;
        int              tileW, tileH;
        int              iconPx;          /* native icon render size */
        int              padTop, padLeft, padRow, padCol;
        int              dotSize, dotActive;   /* page-indicator dots */
        uint32_t         bg;
        const lv_font_t* labelFont;
        const char*      iconRes;         /* icon bucket, e.g. "36x36" */
    } launcher;

    struct NavBar {
        int  h;
        int  btnPx;
        bool defaultHidden;
    } navBar;

    struct Recents {
        int              cardWPct;        /* % of screen width (calibrated -> cardW) */
        int              cardW;           /* resolved px (filled by calibrate) */
        int              iconPx;
        const lv_font_t* titleFont;
        const lv_font_t* subFont;
        uint32_t         subColor;
        int              swipeClosePx;
        int              swipeAngleDeg;
    } recents;

    struct Gesture {
        int vSwipePx;
        int edgePx;
        int angleDeg;
        int shortMs;
        int slowTenthPxPerMs;   /* 0.1px/ms -> 1 (tenths of a px per ms) */
        int detectMs;
        int recentsDwellMs;
    } gesture;
};

/** The active, calibrated stylesheet. Valid after lcdStyleBegin(); before that
 *  it returns the built-in default so early reads are still safe. */
const LcdStyle& lcdStyle(void);

/** Select the registered sheet matching the real panel (w,h) — else the built-in
 *  320x240 default — copy it to the active sheet, and calibrate (resolve
 *  percents to px, sanity-check). Call once from shellInit before any read. */
void lcdStyleBegin(int w, int h);
