/**
 * cli_app.cpp — the on-device CLI program as an LcdApp.
 *
 * Direct port of the legacy CLI program (lcd_apps.cpp): a real VT100/xterm
 * terminal (lcd_term/libvterm) wired to the device CLI session over the cli:1 DC
 * port (CLI_ANSI), so a shell / top / vim / ncurses lay out correctly. The
 * trackball drives arrow keys while the CLI is up (setScrollwheelArrows). The
 * lifecycle maps as for Log: onCreate builds the terminal + opens the connection,
 * onClose closes it and frees the VTerm.
 *
 * As before, ITS recv/disconnect callbacks carry no user pointer, so the single
 * CLI instance's terminal lives in a file-static.
 */
#include "lcd_app.h"
#include "shell_internal.h"
#include "stylesheet.h"
#include "lcd_internal.h"   /* lcdScreenW/H, lcdInputGroup */
#include "lcd_term.h"

#include "its.h"
#include "cli.h"
#include "log.h"

#include <string>
#include <cstring>

namespace {

lv_color_t cliFg() { return lv_color_hex(0xC8E8C8); }   /* pale green */

int bodyH() {
    int homebarH = lcdScreenH() / 10;
    return (lcdScreenH() - lcdStyle().statusBar.h) - homebarH;
}

int         s_handle  = -1;
lcd_term_t* s_cliTerm = nullptr;

void cliOutput(const char* data, size_t len, void* /*user*/) {
    if (s_handle >= 0) itsSend(s_handle, data, len, pdMS_TO_TICKS(200));
}

void cliRecvCb(int handle, size_t) {
    char tmp[2048];
    size_t n;
    while ((n = itsRecv(handle, tmp, sizeof(tmp), 0)) > 0)
        lcdTermFeed(s_cliTerm, tmp, n);
}

void cliDiscCb(int) {
    s_handle = -1;
    const char* m = "\r\n[cli connection closed]\r\n";
    lcdTermFeed(s_cliTerm, m, strlen(m));
}

/* Deferred focus: the launcher tile that opened us grabs the input group on
 * click-release (after our handlers), so an immediate focus is stolen back; a
 * one-shot timer focuses once that settles. Not ledgered — a repeat-count-1
 * timer self-deletes after firing. */
void cliFocus(void) {
    lv_obj_t* c = lcdTermObj(s_cliTerm);
    if (c && lv_obj_is_valid(c)) lv_group_focus_obj(c);
}
void cliFocusTimerCb(lv_timer_t*) { cliFocus(); }
void cliFocusClickCb(lv_event_t*) { cliFocus(); }
void cliKeyCb(lv_event_t* e)      { lcdTermKey(s_cliTerm, lv_event_get_key(e)); }

class CliApp : public LcdApp {
public:
    CliApp() : LcdApp({ .name = "CLI", .iconBasename = "cli" }) {}

    void onCreate(lv_obj_t* root) override {
        s_cliTerm = lcdTermCreate(root, lcdScreenW(), bodyH(),
                                  lcdFont(LcdFace::MONO, 8), cliFg(), cliOutput, nullptr);

        lv_obj_t* cont = lcdTermObj(s_cliTerm);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), cont);
        lv_obj_add_event_cb(cont, cliKeyCb,        LV_EVENT_KEY,     nullptr);
        lv_obj_add_event_cb(cont, cliFocusClickCb, LV_EVENT_CLICKED, nullptr);
        lv_timer_t* ft = lv_timer_create(cliFocusTimerCb, 40, nullptr);
        lv_timer_set_repeat_count(ft, 1);
        setScrollwheelArrows(true);   /* trackball -> arrows while the CLI is up */

        static const char hint[] =
            "Press Alt-C <char> for Ctrl-<char>, Alt-C Alt-C for ESC\r\nTrackball does arrows\r\n";
        lcdTermFeed(s_cliTerm, hint, sizeof(hint) - 1);

        int trows = 0, tcols = 0;
        lcdTermSize(s_cliTerm, &trows, &tcols);
        std::string sz = std::to_string(tcols) + "x" + std::to_string(trows);
        s_handle = itsConnect("cli", CLI_PORT_DC, sz.data(), sz.size(),
                              pdMS_TO_TICKS(500), 0, cliRecvCb, cliDiscCb);
        if (s_handle < 0) {
            const char* m = "[cli: connect failed]\r\n";
            lcdTermFeed(s_cliTerm, m, strlen(m));
        }
    }

    void onClose() override {
        if (s_handle >= 0) itsDisconnect(s_handle);
        lcdTermDestroy(s_cliTerm);   /* frees the VTerm; LVGL objects go with the layer */
        s_cliTerm = nullptr;
        s_handle  = -1;
    }
};

}  // namespace

LcdApp* lcdMakeCliApp(void) { return new CliApp(); }
