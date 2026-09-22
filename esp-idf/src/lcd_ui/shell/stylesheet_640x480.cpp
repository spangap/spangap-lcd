/**
 * stylesheet_640x480.cpp — the sheet for a bare touch panel, 640x480 (the
 * Waveshare 2.8B, whose 480x640 glass is held landscape). The shape is the
 * default's own 4:3, so the tile grid that fits one fits the other and the
 * numbers below are the default's; what this sheet exists to say is that the
 * board has no keys, which is the navigation bar's question.
 *
 * Everything here is in the same REFERENCE pixels as the default sheet — the
 * numbers a 320x240 panel wants — because the shell multiplies every length by
 * the UI zoom (lcdPx) and every font token by the same. A high-density panel
 * asks for a bigger zoom (CONFIG_LCD_UI_SCALE_DEFAULT), not for bigger numbers
 * here; restating them scaled would double-apply.
 */
#include "stylesheet.h"

extern const LcdStyle lcdStyle640x480 = {   /* extern: const has internal linkage by default */
    .name = "touch",
    .displayW = 640,
    .displayH = 480,

    .core = {
        .bg              = 0x101418,
        .fontSpec        = { LcdFace::UI, 14 },
        .font            = nullptr,   /* resolved at calibrate() */
        .monoSpec        = { LcdFace::MONO, 8 },
        .monoFont        = nullptr,   /* resolved at calibrate() */
        .maxResidentApps = 4,
    },

    .statusBar = {
        .h    = 24,
        .bg   = 0x0A2342,
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
        .minTilePx = 72,
        .bg        = 0x101418,
        .labelSpec = { LcdFace::UI, 14 },
        .labelFont = nullptr,   /* resolved at calibrate() */
    },

    .navBar = {
        .h             = 28,
        .btnPx         = 24,
        /* Asked for, unlike the default: the boards that hide the bar have a
         * hardware button to go back and home with, and a bare touch panel has
         * the gesture and nothing else. No chrome reads this yet — the bar is
         * declared here and not built (as LcdApp::Config::navBar is), so today
         * this board navigates by gesture like every other. */
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
