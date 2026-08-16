/**
 * safe_screen.cpp — what the panel says during a safe-mode boot.
 *
 * A safe-mode boot is not a session: it exists to back the state store up,
 * restore one, or erase it, and then restart. Nothing on the launcher applies,
 * and the operator's only question is whether the device is working and how far
 * along it is. So the shell comes up as usual and this covers it — one opaque
 * layer on lv_layer_top, above the status bar and the home-bar strip, with no
 * way past it. What is behind it is never reachable and never meant to be.
 *
 * The wipe is the one operation that can take minutes, and the one that says so:
 * spangap-core publishes `sys.wipe.percent` as it erases, and the bar follows
 * it. It advances in steps rather than smoothly — an SPI-flash erase disables
 * the flash cache, so every task running from flash (this one included) is
 * stopped for the length of each erase op and runs again between them. A bar
 * that moves at all is the answer to "is it stuck?"; a smooth one is not
 * available at any price here.
 *
 * Generic to the platform: safe mode is a spangap-core concept and this is the
 * screen half of it. Nothing here knows what the device is for.
 */
#include "lcd.h"
#include "lcd_internal.h"

#include "spangap.h"
#include "storage.h"

#include <cstdio>

namespace {

lv_obj_t* s_bar = nullptr;
lv_obj_t* s_pct = nullptr;

/* Percentage → bar + readout. Called on the lcd task. */
void applyPercent(void) {
    if (!s_bar) return;
    int pct = storageGetInt("sys.wipe.percent", 0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
    if (s_pct) lv_label_set_text_fmt(s_pct, "%d%%", pct);
}

}  // namespace

void lcdSafeScreenInit(void) {
    safe_mode_t mode = spangapSafeMode();
    if (mode == SAFE_MODE_NONE) return;

    const bool wiping = (mode == SAFE_MODE_FACTORY_RESET);
    const char* head  = wiping                     ? "WIPING FLASH"
                      : mode == SAFE_MODE_BACKUP   ? "BACKING UP"
                                                   : "RESTORING";

    lv_obj_t* ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ov, lcdPx(14), 0);
    lv_obj_set_flex_flow(ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ov, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ov, lcdPx(10), 0);
    /* Clickable and opaque: a stray touch during a wipe must reach nothing. */
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    /* As big as the panel takes. Wrapped and centred rather than sized to fit a
     * particular word: a narrower screen breaks it over two lines instead of
     * running it off the edge. */
    lv_obj_t* h = lv_label_create(ov);
    lv_label_set_text(h, head);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, lv_pct(100));
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(h, lcdFont(LcdFace::UI_BOLD, lcdPx(34)), 0);
    lv_obj_set_style_text_color(h, lv_color_white(), 0);

    if (wiping) {
        s_bar = lv_bar_create(ov);
        lv_obj_set_size(s_bar, lv_pct(86), lcdPx(16));
        lv_bar_set_range(s_bar, 0, 100);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x30363d), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xe06c6c), LV_PART_INDICATOR);

        s_pct = lv_label_create(ov);
        lv_obj_set_style_text_font(s_pct, lcdFont(LcdFace::UI, lcdPx(15)), 0);
        lv_obj_set_style_text_color(s_pct, lv_color_hex(0x8a93a0), 0);
        applyPercent();

        /* The erase is not interruptible and the device restarts itself when it
         * finishes, so the one useful instruction is the one that can still be
         * got wrong. */
        lv_obj_t* warn = lv_label_create(ov);
        lv_label_set_text(warn, "Do not power off.");
        lv_obj_set_style_text_font(warn, lcdFont(LcdFace::UI, lcdPx(13)), 0);
        lv_obj_set_style_text_color(warn, lv_color_hex(0x8a93a0), 0);

        storageSubscribeChanges("sys.wipe.percent", ON_CHANGE {
            (void)key; (void)val;
            lcdRun(ON_LCD { applyPercent(); });
        });
    }
}
