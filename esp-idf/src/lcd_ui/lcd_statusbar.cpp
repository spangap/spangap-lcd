/**
 * lcd_statusbar.cpp — opaque top status bar (on lv_layer_top, always frontmost).
 *
 * Top-left:  date + time via strftime(s.lcd.date_format) (default
 *            "%d %b %Y, %H:%M" -> "22 May 2026, 13:44"), refreshed by a 1 s
 *            lv_timer (the clock inherently needs a tick).
 * Top-right: wifi signal glyph (LV_SYMBOL_WIFI) whose opacity tracks RSSI,
 *            driven event-driven off storage (wifi.sta.state / wifi.sta.rssi,
 *            published by net) — no net call, no polling.
 */
#include "lcd_internal.h"

#include "storage.h"

#include <cstring>
#include <ctime>
#include <string>

namespace {

lv_obj_t* s_bar   = nullptr;
lv_obj_t* s_clock = nullptr;
lv_obj_t* s_wifi  = nullptr;

void updateClock(lv_timer_t* t) {
    char fmt[64];
    storageGetStr("s.lcd.date_format", fmt, sizeof(fmt), "%d %b %Y, %H:%M");
    time_t now = time(nullptr);
    struct tm tm {};
    localtime_r(&now, &tm);
    char buf[80];
    strftime(buf, sizeof(buf), fmt, &tm);
    lv_label_set_text(s_clock, buf);

    /* Refresh only as often as the shown value changes: 1s if the format shows
     * seconds, else aligned to the next minute boundary — no redundant ticks. */
    if (t) {
        bool hasSec = strstr(fmt, "%S") || strstr(fmt, "%T") || strstr(fmt, "%r") ||
                      strstr(fmt, "%c") || strstr(fmt, "%X");
        uint32_t next = hasSec ? 1000u : (uint32_t)(60 - tm.tm_sec) * 1000u;
        if (next < 1000u) next = 1000u;
        lv_timer_set_period(t, next);
        lv_timer_reset(t);
    }
}

/* Show the clock only once the time is known-valid (sys.time.valid, flipped by
 * ntp's sync callback) — never display a 1970 wall clock. Event-driven off the
 * storage subscription below, no polling. */
void updateTimeVisible(const char*, const char*) {
    if (storageGetInt("sys.time.valid", 0)) {
        lv_obj_remove_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
        updateClock(nullptr);                      /* refresh now that it shows */
    } else {
        lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    }
}

/* storage ON_CHANGE handler for wifi.sta.* — re-reads the leaves by key. */
void updateWifi(const char*, const char*) {
    std::string state = storageGetStr("wifi.sta.state", "off");
    int rssi = storageGetInt("wifi.sta.rssi", 0);
    lv_opa_t opa;
    if      (state != "connected") opa = LV_OPA_20;   /* not associated */
    else if (rssi >= -55)          opa = LV_OPA_COVER;
    else if (rssi >= -65)          opa = LV_OPA_80;
    else if (rssi >= -75)          opa = LV_OPA_60;
    else if (rssi >= -85)          opa = LV_OPA_40;
    else                           opa = LV_OPA_30;
    lv_obj_set_style_text_opa(s_wifi, opa, 0);
}

}  // namespace

void lcdStatusbarInit(void) {
    s_bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_size(s_bar, lcdScreenW(), LCD_STATUSBAR_H);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x0A2342), 0);  /* dark navy, not black */
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);

    s_clock = lv_label_create(s_bar);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(s_clock, lv_color_white(), 0);
    lv_label_set_text(s_clock, "");
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);  /* shown once time is valid */

    s_wifi = lv_label_create(s_bar);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(s_wifi, lv_color_white(), 0);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);

    updateClock(nullptr);
    lv_timer_create(updateClock, 1000, nullptr);

    /* Event-driven wifi icon: publishWifiStatus() does storageSet on wifi.sta.* */
    storageSubscribeChanges("wifi.sta", updateWifi);
    updateWifi(nullptr, nullptr);                  /* apply current state once */

    /* Event-driven clock visibility: ntp flips sys.time.valid on first sync. */
    storageSubscribeChanges("sys.time.valid", updateTimeVisible);
    updateTimeVisible(nullptr, nullptr);           /* apply current state once */
}

void lcdStatusbarSetVisible(bool visible) {
    if (!s_bar) return;
    if (visible) lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag   (s_bar, LV_OBJ_FLAG_HIDDEN);
}
