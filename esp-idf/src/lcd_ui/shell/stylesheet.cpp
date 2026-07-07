/**
 * stylesheet.cpp — sheet selection + calibration + theme install. Picks the
 * registered sheet matching the real panel size (else the built-in default),
 * copies it to the active sheet, resolves font tokens + percent fields to px
 * (scaled by the runtime UI zoom and the panel-height ratio), and installs a
 * dark theme carrying the UI font so labels inherit it. Keeping the active sheet
 * a value copy means the rest of the shell reads plain px / resolved font
 * pointers off lcdStyle() with no per-read math.
 */
#include "stylesheet.h"
#include "lcd_fonts.h"
#include "storage.h"
#include "log.h"

#include <cmath>

extern const LcdStyle lcdStyleDefault320x240;

namespace {

/* Registry of known sheets. One today; a per-board sheet is a new entry. */
const LcdStyle* const s_sheets[] = { &lcdStyleDefault320x240 };

LcdStyle s_active = lcdStyleDefault320x240;   /* safe default before begin() */

float s_uiScale = 1.0f;

/* Resolve a font token to a concrete font at the active scale + panel height.
 * Basis is the displayH/240 ratio (driver DPI is too often bogus), times the
 * runtime UI zoom. */
const lv_font_t* resolveFont(const FontSpec& spec, int displayH) {
    float px = (float)spec.basePx * s_uiScale * (float)displayH / 240.0f;
    int ipx = (int)lroundf(px);
    if (ipx < 4) ipx = 4;
    return lcdFont(spec.face, ipx);
}

void calibrate(LcdStyle& s, int w, int h) {
    s.displayW = w;
    s.displayH = h;

    /* UI zoom: s.lcd.scale percent, clamped, as a fraction. */
    int pct = storageGetInt("s.lcd.scale", 100);
    if (pct < 50)  pct = 50;
    if (pct > 200) pct = 200;
    s_uiScale = (float)pct / 100.0f;

    /* Resolve font tokens → concrete fonts. */
    s.core.font        = resolveFont(s.core.fontSpec,     h);
    s.launcher.labelFont = resolveFont(s.launcher.labelSpec, h);
    s.recents.titleFont  = resolveFont(s.recents.titleSpec,  h);
    s.recents.subFont    = resolveFont(s.recents.subSpec,    h);

    /* Resolve percents to px. Only the recents card is a percent today. */
    s.recents.cardW = (w * s.recents.cardWPct) / 100;
}

/* Install a dark theme wrapping lv_theme_default, carrying the UI font so every
 * label inherits it (font inheritance — only title/statusbar/mono override). */
void installTheme(const LcdStyle& s) {
#if LV_USE_THEME_DEFAULT
    lv_display_t* disp = lv_display_get_default();
    if (!disp) return;
    const lv_font_t* uiFont = s.core.font ? s.core.font : LV_FONT_DEFAULT;
    lv_theme_t* th = lv_theme_default_init(disp,
                                           lv_color_hex(0x2563a0),   /* primary */
                                           lv_color_hex(0x36C06A),   /* secondary */
                                           /*dark=*/true,
                                           uiFont);
    lv_display_set_theme(disp, th);
#endif
}

}  // namespace

const LcdStyle& lcdStyle(void) { return s_active; }
float           lcdUiScale(void) { return s_uiScale; }

void lcdStyleBegin(int w, int h) {
    const LcdStyle* pick = &lcdStyleDefault320x240;
    for (auto* s : s_sheets)
        if (s->displayW == w && s->displayH == h) { pick = s; break; }
    s_active = *pick;
    calibrate(s_active, w, h);
    installTheme(s_active);
    info("stylesheet '%s' for %dx%d (zoom %d%%, recents card %dpx)\n",
         s_active.name, w, h, (int)lroundf(s_uiScale * 100), s_active.recents.cardW);
}

void lcdStyleRecalibrate(void) {
    calibrate(s_active, s_active.displayW, s_active.displayH);
    installTheme(s_active);
}
