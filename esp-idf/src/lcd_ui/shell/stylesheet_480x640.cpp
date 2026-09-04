/**
 * stylesheet_480x640.cpp — the portrait sheet, for a 480x640 panel held tall
 * (the Waveshare 2.8B). It differs from the 320x240 default in the two things
 * the shape and the hardware actually change, and in nothing else: how many
 * rows of tiles a viewport twice as tall should hold, and whether the
 * navigation bar is on screen.
 *
 * Everything here is in the same REFERENCE pixels as the default sheet — the
 * numbers a 320x240 panel wants — because the shell multiplies every length by
 * the UI zoom (lcdPx) and every font token by the same. A high-density panel
 * asks for a bigger zoom (CONFIG_LCD_UI_SCALE_DEFAULT), not for bigger numbers
 * here; restating them scaled would double-apply.
 */
#include "stylesheet.h"

extern const LcdStyle lcdStyle480x640 = {   /* extern: const has internal linkage by default */
    .name = "portrait",
    .displayW = 480,
    .displayH = 640,

    .core = {
        .bg              = 0x101418,
        .fontSpec        = { LcdFace::UI, 14 },
        .font            = nullptr,   /* resolved at calibrate() */
        .maxResidentApps = 4,
    },

    .statusBar = {
        .h    = 24,
        .bg   = 0x0A2342,
        .text = 0xFFFFFF,
    },

    .launcher = {
        .cols      = 4,
        /* Five rows, not three: the tile height is the viewport divided by this,
         * so the default sheet's three would draw a 4-wide grid of tiles half
         * again as tall as they are wide. Five is what keeps them square on a
         * panel of this aspect. */
        .rows      = 5,
        .tileW     = 72,
        .tileH     = 64,
        .iconPx    = 36,
        .padTop    = 8,
        .padLeft   = 8,
        .padRow    = 8,
        .padCol    = 8,
        .minTilePx = 72,
        .bg        = 0x101418,
        .labelSpec = { LcdFace::UI, 14 },
        .labelFont = nullptr,   /* resolved at calibrate() */
    },

    .navBar = {
        .h             = 28,
        .btnPx         = 24,
        /* Shown, unlike the default. The boards that hide it have a hardware
         * button to go back and home with; a bare touch panel has the gesture
         * and nothing else, and a gesture is not discoverable. */
        .defaultHidden = false,
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
