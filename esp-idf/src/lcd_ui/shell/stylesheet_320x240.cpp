/**
 * stylesheet_320x240.cpp — the one shipped sheet: 320x240 dark, on spangap's own
 * bitmap fonts/icons. Values are the launcher's current pixel constants made
 * data: status bar 24px (lcd_internal.h LCD_STATUSBAR_H), 4x3 tile grid 72x64
 * with 8px pads and a 36px icon (lcd_launcher.cpp), bg 0x101418, status-bar navy
 * 0x0A2342, icon bucket "36x36" (lcd_icons.cpp LAUNCHER_ICON_RES). Recents/nav/
 * gesture carry the donor's 320x240 thresholds (plan §5.4–5.6) for M3.
 */
#include "stylesheet.h"
#include "lcd.h"   /* lv_font_montserrat_12_latin / _16_latin */

extern const LcdStyle lcdStyleDefault320x240 = {   /* extern: const has internal linkage by default */
    .name = "default",
    .displayW = 320,
    .displayH = 240,

    .core = {
        .bg              = 0x101418,
        .font            = &lv_font_montserrat_12_latin,
        .titleFont       = &lv_font_montserrat_16_latin,
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
        .bg        = 0x101418,
        .labelFont = &lv_font_montserrat_12_latin,
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
        .titleFont     = &lv_font_montserrat_12_latin,
        .subFont       = &lv_font_montserrat_12_latin,
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
