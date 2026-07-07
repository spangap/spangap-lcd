/**
 * stylesheet_320x240.cpp — the one shipped sheet: 320x240 dark. Fonts are now
 * TOKENS ({face, basePx}); calibrate() resolves them through lcdFont() so the
 * shell renders vector faces at any size/zoom (or bitmaps under LCD_FONT_BITMAP).
 * Geometry values are the launcher's current pixel constants made data: status
 * bar 24px, tiles derived from a 72px floor, bg 0x101418, status-bar navy
 * 0x0A2342. Recents/nav/gesture carry the donor's 320x240 thresholds.
 */
#include "stylesheet.h"

extern const LcdStyle lcdStyleDefault320x240 = {   /* extern: const has internal linkage by default */
    .name = "default",
    .displayW = 320,
    .displayH = 240,

    .core = {
        .bg              = 0x101418,
        .fontSpec        = { LcdFace::UI, 14 },   /* 14 = smallest UI face */
        .font            = nullptr,   /* resolved at calibrate() */
        .maxResidentApps = 4,
    },

    .statusBar = {
        .h    = 24,
        .bg   = 0x0A2342,   /* dark navy, not black */
        .text = 0xFFFFFF,
    },

    .launcher = {
        .cols      = 4,
        .rows      = 3,
        .tileW     = 72,
        .tileH     = 64,
        .iconPx    = 36,
        .padTop    = 8,
        .padLeft   = 8,
        .padRow    = 8,
        .padCol    = 8,
        .dotSize   = 8,
        .dotActive = 20,
        .minTilePx = 72,    /* cols = floor(usableW / minTilePx*uiScale) */
        .bg        = 0x101418,
        .labelSpec = { LcdFace::UI, 14 },
        .labelFont = nullptr,   /* resolved at calibrate() */
        .iconRes   = "36x36",
    },

    .navBar = {
        .h             = 28,
        .btnPx         = 24,
        .defaultHidden = true,   /* T-Deck: gesture + ESC + Home button cover it */
    },

    .recents = {
        .cardWPct      = 60,
        .cardW         = 0,      /* calibrated from cardWPct at begin() */
        .iconPx        = 36,
        .titleSpec     = { LcdFace::UI, 14 },
        .subSpec       = { LcdFace::UI, 14 },
        .titleFont     = nullptr,   /* resolved at calibrate() */
        .subFont       = nullptr,
        .subColor      = 0x9098A0,
        .swipeClosePx  = 30,
        .swipeAngleDeg = 60,
    },

    .gesture = {
        .vSwipePx         = 50,
        .edgePx           = 20,
        .angleDeg         = 60,
        .shortMs          = 800,
        .slowTenthPxPerMs = 1,   /* 0.1 px/ms */
        .detectMs         = 20,
        .recentsDwellMs   = 400,
    },
};
