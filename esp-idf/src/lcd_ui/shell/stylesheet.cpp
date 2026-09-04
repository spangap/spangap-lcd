/**
 * stylesheet.cpp — sheet selection + calibration + theme install. Picks the
 * registered sheet matching the real panel size (else the built-in default),
 * copies it to the active sheet, resolves font tokens + percent fields to px
 * (scaled by the runtime UI zoom), and installs a
 * dark theme carrying the UI font so labels inherit it. Keeping the active sheet
 * a value copy means the rest of the shell reads plain px / resolved font
 * pointers off lcdStyle() with no per-read math.
 */
#include "stylesheet.h"
#include "lcd_fonts.h"
#include "storage.h"
#include "log.h"
#include "sdkconfig.h"    /* CONFIG_LCD_UI_SCALE_DEFAULT */

#include <cmath>

extern const LcdStyle lcdStyleDefault320x240;
extern const LcdStyle lcdStyle480x640;

namespace {

/* Registry of known sheets, matched on the panel's post-rotation size. A panel
 * nobody wrote a sheet for gets the 320x240 default, which is written in
 * proportions rather than in absolute positions and so holds its shape at any
 * size the zoom puts it at. */
const LcdStyle* const s_sheets[] = { &lcdStyleDefault320x240, &lcdStyle480x640 };

LcdStyle s_active = lcdStyleDefault320x240;   /* safe default before begin() */

float s_uiScale = 1.0f;

/* Resolve a font token to a concrete font at the active scale.
 *
 * ONE number scales the shell. Every length the UI states goes through lcdPx()
 * (× s_uiScale) and every font size through here, and they are multiplied by
 * exactly the same thing — so a sheet's proportions survive any zoom, which
 * they do not if the type scales on a second, panel-derived factor and the
 * padding around it does not. What a bigger panel is worth in zoom is a
 * judgement about the GLASS (pixel pitch, how far away it is held) that no
 * ratio of pixel counts answers, so it is stated: CONFIG_LCD_UI_SCALE_DEFAULT
 * per board, s.lcd.scale per operator. */
const lv_font_t* resolveFont(const FontSpec& spec) {
    int ipx = (int)lroundf((float)spec.basePx * s_uiScale);
    if (ipx < 4) ipx = 4;
    return lcdFont(spec.face, ipx);
}

void calibrate(LcdStyle& s, int w, int h) {
    s.displayW = w;
    s.displayH = h;

    /* UI zoom: s.lcd.scale percent, clamped, as a fraction. */
    int pct = storageGetInt("s.lcd.scale", CONFIG_LCD_UI_SCALE_DEFAULT);
    if (pct < 50)  pct = 50;
    if (pct > 250) pct = 250;
    s_uiScale = (float)pct / 100.0f;

    /* Resolve font tokens → concrete fonts. */
    s.core.font          = resolveFont(s.core.fontSpec);
    s.launcher.labelFont = resolveFont(s.launcher.labelSpec);
    s.recents.titleFont  = resolveFont(s.recents.titleSpec);
    s.recents.subFont    = resolveFont(s.recents.subSpec);

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
int             lcdPx(int px)    { return (int)(px * s_uiScale + 0.5f); }

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
