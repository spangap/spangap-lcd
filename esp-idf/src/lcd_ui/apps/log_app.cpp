/**
 * log_app.cpp — the on-device Log program as an LcdApp (the first real one).
 *
 * Direct port of the legacy Log program (lcd_apps.cpp): a virtualized text view
 * in Spleen 5x8, an ITS client of the log task's log:1 DC port (plain text,
 * {"ansi":0}), with per-line severity colour. The lifecycle maps cleanly:
 *   onCreate  <- build the text view + open the ITS connection (was logFn)
 *   onShow    <- scroll to the tail (newest output)
 *   onClose   <- close the ITS connection (was the LV_EVENT_DELETE cb)
 * The connection lives for the life of the layer, not its visibility, so a Log
 * hidden behind another app keeps receiving and re-opening shows current state.
 *
 * ITS recv/disconnect callbacks carry no user pointer, so the connection state
 * lives in a file-static (there is one Log instance, as before).
 */
#include "lcd_app.h"
#include "shell_internal.h"
#include "stylesheet.h"
#include "lcd_internal.h"   /* lcdScreenW/H */

#include "its.h"
#include "log.h"
#include "storage.h"

#include <cstring>

namespace {

lv_color_t logFg() { return lv_color_hex(0xC8C8C8); }   /* light grey */

uint32_t logLineColor(const char* s, size_t n) {
    for (size_t k = 0; k + 2 < n && k < 28; k++) {
        if (k > 0 && s[k - 1] != ' ') continue;
        if (s[k + 1] != ' ' || s[k + 2] != '[') continue;
        switch (s[k]) {
            case 'E': return 0xE06A6A;
            case 'W': return 0xD8B860;
            case 'I': return LCD_TEXTVIEW_DEFAULT;
            case 'D': return 0x8890A0;
            case 'V': return 0x687078;
            default:  break;
        }
    }
    return LCD_TEXTVIEW_DEFAULT;
}

size_t scrollbackBudget() {
    long kb = storageGetInt("s.log.file.paste", 48);
    if (kb < 4) kb = 4;
    return (size_t)kb * 1024;
}

/* Body height: layer (already minus the status bar) minus the home-bar strip. */
int bodyH() {
    int homebarH = lcdScreenH() / 10;
    return (lcdScreenH() - lcdStyle().statusBar.h) - homebarH;
}

struct State { int handle = -1; lcd_textview_t* tv = nullptr; bool primed = false; };
State s_log;

void logRecvCb(int handle, size_t) {
    char tmp[2048];
    size_t n;
    while ((n = itsRecv(handle, tmp, sizeof(tmp), 0)) > 0) {
        if (!s_log.tv) continue;
        if (!s_log.primed) { lcdTextViewSet(s_log.tv, tmp, n); s_log.primed = true; }
        else                 lcdTextViewAppend(s_log.tv, tmp, n);
    }
}

void logDiscCb(int) {
    s_log.handle = -1;
    if (s_log.tv) {
        const char* m = "\n[log connection closed]\n";
        lcdTextViewAppend(s_log.tv, m, strlen(m));
    }
}

class LogApp : public LcdApp {
public:
    LogApp() : LcdApp({ .name = "Log", .iconBasename = "log" }) {}

    void onCreate(lv_obj_t* root) override {
        s_log.tv = lcdTextViewCreate(root, lcdScreenW(), bodyH(),
                                     &lv_font_spleen_5x8, logFg(), scrollbackBudget());
        lcdTextViewSetLineColor(s_log.tv, logLineColor);
        s_log.primed = false;

        static const char proto[] = "{\"ansi\":0}";
        s_log.handle = itsConnect("log", LOG_PORT_DC, proto, sizeof(proto) - 1,
                                  pdMS_TO_TICKS(500), 0, logRecvCb, logDiscCb);
        const char* hint = (s_log.handle < 0) ? "[log: connect failed]"
                                              : "(waiting for log output)";
        lcdTextViewSet(s_log.tv, hint, strlen(hint));
    }

    void onShow() override {
        if (s_log.tv) lcdTextViewScrollToBottom(s_log.tv);
    }

    void onClose() override {
        if (s_log.handle >= 0) itsDisconnect(s_log.handle);
        s_log = State{};   /* the text view frees itself with the layer tree */
    }
};

}  // namespace

LcdApp* lcdMakeLogApp(void) { return new LogApp(); }
