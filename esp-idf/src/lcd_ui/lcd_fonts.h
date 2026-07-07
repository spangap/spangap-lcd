/**
 * lcd_fonts.h — internal font-engine hooks (implementation in lcd_fonts.cpp).
 *
 * The PUBLIC font API — enum class LcdFace, lcdFont(), lcdFontsReset() — lives in
 * lcd.h so any straddle's lcd/ slice can render vector faces. This header adds
 * only the private bring-up hook. See lcd_fonts.cpp for the engine variants
 * (FreeType / tiny_ttf / bitmap) behind CONFIG_LCD_FONT_ENGINE.
 */
#pragma once

#include "lcd.h"   /* LcdFace, lcdFont, lcdFontsReset */

/** Font-engine bring-up hook. Call once after lv_init(), before the first
 *  lcdFont(). A no-op today (lv_init() brings FreeType up; tiny_ttf needs no
 *  library init), kept as the documented seam. */
void lcdFontsInit(void);
