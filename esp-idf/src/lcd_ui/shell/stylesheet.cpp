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
extern const LcdStyle lcdStyle640x480;

namespace {

/* Registry of known sheets, matched on the panel's post-rotation size. A panel
 * nobody wrote a sheet for gets the 320x240 default, which is written in
 * proportions rather than in absolute positions and so holds its shape at any
 * size the zoom puts it at. */
const LcdStyle* const s_sheets[] = { &lcdStyleDefault320x240, &lcdStyle640x480 };

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
    s.core.monoFont      = resolveFont(s.core.monoSpec);
    s.launcher.labelFont = resolveFont(s.launcher.labelSpec);
    s.recents.titleFont  = resolveFont(s.recents.titleSpec);
    s.recents.subFont    = resolveFont(s.recents.subSpec);

    /* Resolve percents to px. Only the recents card is a percent today. */
    s.recents.cardW = (w * s.recents.cardWPct) / 100;

    /* Every length the sheet states is in REFERENCE pixels; the active sheet
     * carries DEVICE pixels. Resolving them all here, once, is what keeps a
     * field from being read raw at one call site and scaled at another — which
     * is a status bar of the shipped 24 px under type that grew with the zoom. */
    s.statusBar.h        = lcdPx(s.statusBar.h);
    s.launcher.tileW     = lcdPx(s.launcher.tileW);
    s.launcher.tileH     = lcdPx(s.launcher.tileH);
    s.launcher.iconPx    = lcdPx(s.launcher.iconPx);
    s.launcher.padTop    = lcdPx(s.launcher.padTop);
    s.launcher.padLeft   = lcdPx(s.launcher.padLeft);
    s.launcher.padRow    = lcdPx(s.launcher.padRow);
    s.launcher.padCol    = lcdPx(s.launcher.padCol);
    s.launcher.minTilePx = lcdPx(s.launcher.minTilePx);
    s.navBar.h           = lcdPx(s.navBar.h);
    s.navBar.btnPx       = lcdPx(s.navBar.btnPx);
    s.recents.iconPx     = lcdPx(s.recents.iconPx);
    s.recents.swipeClosePx = lcdPx(s.recents.swipeClosePx);
    s.gesture.vSwipePx   = lcdPx(s.gesture.vSwipePx);
    s.gesture.edgePx     = lcdPx(s.gesture.edgePx);
}

/* Install a dark theme wrapping lv_theme_default, carrying the UI font so every
 * label inherits it (font inheritance — only title/statusbar/mono override). */
void installTheme(const LcdStyle& s) {
#if LV_USE_THEME_DEFAULT
    lv_display_t* disp = lv_display_get_default();
    if (!disp) return;
    /* Everything LVGL sizes for itself — scrollbar widths, dropdown and button
     * paddings, knobs, corner radii — comes from the display's DPI, not from
     * our zoom. Left at the built-in default it stays deck-sized under text
     * that has grown, which reads as a widget that has shrunk. The theme reads
     * this at init, so it is set before the init below. */
    lv_display_set_dpi(disp, (int)lroundf((float)LV_DPI_DEF * s_uiScale));
    const lv_font_t* uiFont = s.core.font ? s.core.font : LV_FONT_DEFAULT;
    lv_theme_t* th = lv_theme_default_init(disp,
                                           lv_color_hex(0x2563a0),   /* primary */
                                           lv_color_hex(0x36C06A),   /* secondary */
                                           /*dark=*/true,
                                           uiFont);
    lv_display_set_theme(disp, th);

    /* The theme reaches an object when it is created and the shell strips it
     * off again wherever it draws its own chrome (lv_obj_remove_style_all, in
     * fifty places). A label under one of those asks its parents for a font,
     * finds none, and lands on LVGL's compiled-in default — a fixed size that
     * ignores the zoom, so it SHRINKS as the display grows. The font is
     * inherited, so putting it on the layers themselves puts a scaled font at
     * the root of every chain, whatever is stripped in between. */
    lv_obj_t* const roots[] = { lv_screen_active(), lv_layer_top(),
                                lv_layer_sys(), lv_layer_bottom() };
    for (lv_obj_t* r : roots)
        if (r) lv_obj_set_style_text_font(r, uiFont, 0);
#endif
}

}  // namespace

const LcdStyle& lcdStyle(void) { return s_active; }
float           lcdUiScale(void) { return s_uiScale; }
int             lcdPx(int px)    { return (int)(px * s_uiScale + 0.5f); }
int             lcdStatusBarH(void) { return s_active.statusBar.h; }
const lv_font_t* lcdFontMono(void) { return s_active.core.monoFont; }

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
