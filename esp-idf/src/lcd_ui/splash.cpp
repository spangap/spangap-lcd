/**
 * splash.cpp — what the panel shows while the device is still assembling itself.
 *
 * A boot is not instant: the shell is built, then every straddle's onInit
 * installs its launcher tile, and each tile's icon is rasterized from SVG as it
 * lands. Until all of that has happened the launcher is a grid with holes in it
 * — so the screen carries this instead: the firmware's name and a word saying
 * it is working, centred on an opaque layer, and the backlight comes up on THIS
 * rather than on a half-built launcher.
 *
 * It sits on the system layer, above lv_layer_top, so it covers the status bar,
 * the home-bar strip, the safe-mode screen and the keyboard that is preloaded
 * behind it — everything, whatever order those were created in. It takes clicks
 * rather than letting them through to a shell that is not ready for them.
 *
 * The dismissal is lcd_lvgl.cpp's boot reveal, which waits for the boot walk to
 * finish AND the icon loads to go quiet before fading this out — so the launcher
 * is complete the moment it is first seen.
 *
 * Generic to the platform: the name is the firmware identity spangap-core
 * publishes (CONFIG_SPANGAP_FW_NAME), so nothing here knows what the device is.
 */
#include "lcd.h"
#include "lcd_internal.h"

#include "sdkconfig.h"      /* CONFIG_SPANGAP_FW_NAME — whose boot this is */

namespace {

lv_obj_t* s_splash = nullptr;

/* Long enough to read as a fade rather than a cut, short enough that nobody
 * waits for it: the launcher underneath is already finished and standing still. */
constexpr uint32_t kFadeMs = 250;

}  // namespace

void lcdSplashShow(void) {
    if (s_splash) return;

    lv_obj_t* ov = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ov, lcdPx(14), 0);
    lv_obj_set_flex_flow(ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ov, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ov, lcdPx(10), 0);
    /* Opaque to touch as well as to light: a tap during boot must reach nothing. */
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    /* Wrapped and centred rather than sized to fit a particular name: a long one
     * on a narrow panel breaks over two lines instead of running off the edge. */
    lv_obj_t* name = lv_label_create(ov);
    lv_label_set_text(name, CONFIG_SPANGAP_FW_NAME);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(name, lcdFont(LcdFace::UI_BOLD, lcdPx(34)), 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);

    lv_obj_t* sub = lv_label_create(ov);
    lv_label_set_text(sub, "Loading...");
    lv_obj_set_style_text_font(sub, lcdFont(LcdFace::UI, lcdPx(20)), 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8a93a0), 0);

    s_splash = ov;
    lcdBootSplashLit();   /* paint it, then light the panel on it */
}

bool lcdSplashActive(void) { return s_splash != nullptr; }

void lcdSplashDismiss(void) {
    if (!s_splash) return;
    lv_obj_t* ov = s_splash;
    s_splash = nullptr;
    /* Fade the layer out, then delete it: LVGL's opacity style carries the whole
     * subtree, so the two labels go with it. The delete rides a one-shot timer
     * because the fade has no completion callback of its own. */
    lv_obj_fade_out(ov, kFadeMs, 0);
    lv_timer_t* t = lv_timer_create([](lv_timer_t* tm) {
        lv_obj_delete(static_cast<lv_obj_t*>(lv_timer_get_user_data(tm)));
    }, kFadeMs + 30, ov);
    lv_timer_set_repeat_count(t, 1);
}
