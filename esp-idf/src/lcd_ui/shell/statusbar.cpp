/**
 * statusbar.cpp — the shell's opaque top status bar (on lv_layer_top, always
 * frontmost). A renderer fed from spangap storage keys, event-driven (no
 * polling): clock from s.lcd.date_format, wifi from wifi.sta.*, battery from
 * battery.percent. Geometry/colour come from the stylesheet, not #defines.
 *
 * M1 ports the legacy bar (clock + wifi opacity + battery) onto the stylesheet so
 * the shell does not regress. M2 adds the upstream-reachability glyph
 * (wifi.sta.up), RSSI bars (replacing the opacity hack), and the per-app icon
 * areas (LcdApp::setStatusIcon) behind the same renderer.
 */
#include "shell_internal.h"
#include "stylesheet.h"
#include "lcd_internal.h"

#include "storage.h"

#include <cstring>
#include <ctime>
#include <string>

namespace {

lv_obj_t* s_bar   = nullptr;
lv_obj_t* s_clock = nullptr;
lv_obj_t* s_wifi  = nullptr;
lv_obj_t* s_batt  = nullptr;
lv_obj_t* s_up    = nullptr;   /* upstream-reachable glyph (wifi.sta.up) */

void updateClock(lv_timer_t* t) {
    char fmt[64];
    storageGetStr("s.lcd.date_format", fmt, sizeof(fmt), "%d %b %Y, %H:%M");
    time_t now = time(nullptr);
    struct tm tm {};
    localtime_r(&now, &tm);
    char buf[80];
    strftime(buf, sizeof(buf), fmt, &tm);
    lv_label_set_text(s_clock, buf);
    if (t) {
        bool hasSec = strstr(fmt, "%S") || strstr(fmt, "%T") || strstr(fmt, "%r") ||
                      strstr(fmt, "%c") || strstr(fmt, "%X");
        uint32_t next = hasSec ? 1000u : (uint32_t)(60 - tm.tm_sec) * 1000u;
        if (next < 1000u) next = 1000u;
        lv_timer_set_period(t, next);
        lv_timer_reset(t);
    }
}

void updateTimeVisible(const char*, const char*) {
    if (storageGetInt("sys.time.valid", 0)) {
        lv_obj_remove_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
        updateClock(nullptr);
    } else {
        lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    }
}

void updateWifi(const char*, const char*) {
    std::string state = storageGetStr("wifi.sta.state", "off");
    int rssi = storageGetInt("wifi.sta.rssi", 0);
    lv_opa_t opa;
    if      (state != "connected") opa = LV_OPA_20;
    else if (rssi >= -55)          opa = LV_OPA_COVER;
    else if (rssi >= -65)          opa = LV_OPA_80;
    else if (rssi >= -75)          opa = LV_OPA_60;
    else if (rssi >= -85)          opa = LV_OPA_40;
    else                           opa = LV_OPA_30;
    lv_obj_set_style_text_opa(s_wifi, opa, 0);
}

/* Upstream reachability: lit (green) when the device has working internet
 * (wifi.sta.up == NET_EV_UPSTREAM_UP for subscribers), dim grey when only
 * associated / AP-only / down. Distinct from the wifi glyph, which only reports
 * association. A small recolored dot — cheap glyph, no new bitmap asset. */
void updateUpstream(const char*, const char*) {
    bool up = storageGetInt("wifi.sta.up", 0) != 0;
    lv_obj_set_style_bg_color(s_up, lv_color_hex(up ? 0x36C06A : 0x404850), 0);
}

void updateBattery(const char*, const char*) {
    if (!storageExists("battery.percent")) {
        lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int pct = storageGetInt("battery.percent", 0);
    const char* glyph;
    if      (pct >= 88) glyph = LV_SYMBOL_BATTERY_FULL;
    else if (pct >= 63) glyph = LV_SYMBOL_BATTERY_3;
    else if (pct >= 38) glyph = LV_SYMBOL_BATTERY_2;
    else if (pct >= 13) glyph = LV_SYMBOL_BATTERY_1;
    else                glyph = LV_SYMBOL_BATTERY_EMPTY;
    lv_label_set_text(s_batt, glyph);
    lv_obj_set_style_text_color(s_batt,
        pct <= 12 ? lv_color_hex(0xFF5555) : lv_color_white(), 0);
    lv_obj_remove_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

void lcdStatusbarInit(void) {
    const LcdStyle& st = lcdStyle();

    s_bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_size(s_bar, lcdScreenW(), st.statusBar.h);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(st.statusBar.bg), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);

    s_clock = lv_label_create(s_bar);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(st.statusBar.text), 0);
    if (st.core.font) lv_obj_set_style_text_font(s_clock, st.core.font, 0);
    lv_label_set_text(s_clock, "");
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);   /* shown once time is valid */

    /* Right cluster (upstream, battery, wifi), right-aligned in a flex row so a
     * hidden element (e.g. battery on a board without one) collapses cleanly. */
    lv_obj_t* cluster = lv_obj_create(s_bar);
    lv_obj_remove_style_all(cluster);
    lv_obj_set_height(cluster, st.statusBar.h);
    lv_obj_set_width(cluster, LV_SIZE_CONTENT);
    lv_obj_align(cluster, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_flex_flow(cluster, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cluster, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cluster, 6, 0);
    lv_obj_remove_flag(cluster, LV_OBJ_FLAG_SCROLLABLE);

    s_up = lv_obj_create(cluster);
    lv_obj_remove_style_all(s_up);
    lv_obj_set_size(s_up, 6, 6);
    lv_obj_set_style_radius(s_up, 3, 0);
    lv_obj_set_style_bg_opa(s_up, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_up, lv_color_hex(0x404850), 0);

    s_batt = lv_label_create(cluster);
    lv_obj_set_style_text_color(s_batt, lv_color_hex(st.statusBar.text), 0);
    lv_label_set_text(s_batt, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);

    s_wifi = lv_label_create(cluster);
    lv_obj_set_style_text_color(s_wifi, lv_color_hex(st.statusBar.text), 0);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);

    updateClock(nullptr);
    lv_timer_create(updateClock, 1000, nullptr);

    storageSubscribeChanges("wifi.sta", updateWifi);
    updateWifi(nullptr, nullptr);
    storageSubscribeChanges("wifi.sta.up", updateUpstream);
    updateUpstream(nullptr, nullptr);
    storageSubscribeChanges("battery.percent", updateBattery);
    updateBattery(nullptr, nullptr);
    storageSubscribeChanges("sys.time.valid", updateTimeVisible);
    updateTimeVisible(nullptr, nullptr);
}

void lcdStatusbarSetVisible(bool visible) {
    if (!s_bar) return;
    if (visible) lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag   (s_bar, LV_OBJ_FLAG_HIDDEN);
}
