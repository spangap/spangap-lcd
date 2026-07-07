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
#include "lcd_fonts.h"   /* LcdFace */
#include <cstdint>

/* A font as a token: a face + a base pixel size at the 240px-tall reference
 * panel. calibrate() resolves it to a concrete lv_font_t* via lcdFont(), scaling
 * the size by the runtime UI zoom and the panel-height ratio — so the resolved
 * pointer fields below are OUTPUTS (filled at lcdStyleBegin), and the sheets set
 * only the specs. On BITMAP builds lcdFont() maps the spec to a bitmap. */
struct FontSpec { LcdFace face; int basePx; };

struct LcdStyle {
    const char* name;        /* "default" / a board name */
    int         displayW;    /* the panel size this sheet targets (0 = any) */
    int         displayH;

    struct Core {
        uint32_t         bg;              /* base background (launcher) */
        FontSpec         fontSpec;        /* default label/clock/status font */
        const lv_font_t* font;            /* resolved (calibrate) */
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
        int              minTilePx;       /* smallest tile edge; cols = usableW/minTile */
        uint32_t         bg;
        FontSpec         labelSpec;
        const lv_font_t* labelFont;       /* resolved (calibrate) */
        const char*      iconRes;         /* icon bucket, e.g. "36x36" (legacy; unused by nanosvg) */
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
        FontSpec         titleSpec;
        FontSpec         subSpec;
        const lv_font_t* titleFont;       /* resolved (calibrate) */
        const lv_font_t* subFont;         /* resolved (calibrate) */
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
 *  320x240 default — copy it to the active sheet, calibrate (resolve font tokens
 *  + percents to px), and install the shell theme (primary colour + dark + the
 *  UI font, so labels inherit it). Call once from shellInit before any read. */
void lcdStyleBegin(int w, int h);

/** The current UI zoom as a fraction (s.lcd.scale%, clamped 50–200 → 0.5–2.0).
 *  calibrate() multiplies every token size and the launcher geometry by it. */
float lcdUiScale(void);

/** Re-run calibration against the active panel and re-install the theme — after
 *  a zoom change (s.lcd.scale). The caller resets the font/icon caches first,
 *  then refreshes widgets (lv_obj_report_style_change). */
void lcdStyleRecalibrate(void);
