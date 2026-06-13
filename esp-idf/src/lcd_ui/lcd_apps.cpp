/**
 * lcd_apps.cpp — built-in on-device Log + CLI launcher programs.
 *
 * Two platform programs that mirror the browser's Log and CLI windows on the
 * device screen. Both are thin terminals in the Spleen 5x8 monospace font: the
 * Log program streams the log task's output; the CLI program is an interactive
 * shell. The scrollable text body is the reusable lcdTextView (lcd.h) — a
 * virtualized view that holds the full scrollback but only ever lays out the
 * visible window, so scrolling and streaming stay smooth at any depth.
 *
 * Lifecycle — the key design point. The lcd task is itself an ITS *client*
 * here (itsClientInit in lcd.cpp); each program opens its connection when its
 * layer is first built and keeps it for the LIFE OF THE LAYER, not its
 * visibility. So a program hidden behind another keeps receiving and updating
 * its (offscreen) widgets — re-opening it shows current state. The connection
 * is bound to the layer via LV_EVENT_DELETE: when the launcher evicts a layer
 * (lv_obj_del under memory pressure), LVGL fires DELETE, we close the ITS
 * connection (the text view frees itself on its own container's DELETE), and
 * the next open rebuilds + reconnects from scratch. recv/disconnect callbacks
 * run on the lcd task (dispatched by its itsPoll loop), so they touch LVGL
 * directly and are guarded by the view handle being non-null.
 *
 * Both connect to the packet-mode DC ports. Log connects with {"ansi":0} —
 * plain text keeps the text view's column math escape-free; severity colours
 * are applied per line at render time instead (lcdTextViewSetLineColor +
 * logLineColor below). The CLI session runs in CLI_ANSI: lcd_term's libvterm
 * parses the SGR colours and the terminal renders them per cell run.
 * Scrollback is capped to s.log.file.paste kB — the same knob that sizes the
 * log paste-back.
 */
#include "lcd_internal.h"
#include "lcd_term.h"

#include "its.h"
#include "log.h"
#include "cli.h"
#include "storage.h"

#include <string>
#include <cstring>

namespace {

lv_color_t logFg() { return lv_color_hex(0xC8C8C8); }   /* light grey */
lv_color_t cliFg() { return lv_color_hex(0xC8E8C8); }   /* pale green */

/* Per-line severity colour for the Log view. Lines are the plain-mode log
 * format "L [task] tag: msg" with an optional wall-clock prefix
 * ("Mar 27 16:23:15.342 ", ~20 chars — log.cpp logReformat), so scan past
 * it for a word-bounded level letter followed by " [" (lineLevel()'s
 * heuristic, tightened by the bracket). Info keeps the default fg (it's
 * the bulk); anomalies and debug stand out. */
uint32_t logLineColor(const char* s, size_t n) {
    for (size_t k = 0; k + 2 < n && k < 28; k++) {
        if (k > 0 && s[k - 1] != ' ') continue;       /* level char is its own word */
        if (s[k + 1] != ' ' || s[k + 2] != '[') continue;
        switch (s[k]) {
            case 'E': return 0xE06A6A;   /* red */
            case 'W': return 0xD8B860;   /* yellow */
            case 'I': return LCD_TEXTVIEW_DEFAULT;
            case 'D': return 0x8890A0;   /* dimmed blue-grey */
            case 'V': return 0x687078;   /* dimmer still */
            default:  break;
        }
    }
    return LCD_TEXTVIEW_DEFAULT;
}

/* Scrollback budget in bytes, from the shared backlog knob (kB), floored. */
size_t scrollbackBudget() {
    long kb = storageGetInt("s.log.file.paste", 48);
    if (kb < 4) kb = 4;
    return (size_t)kb * 1024;
}

/* Terminal body height: screen minus the status bar and the home-bar strip. */
int termBodyH() {
    int homebarH = lcdScreenH() / 10;
    return (lcdScreenH() - LCD_STATUSBAR_H) - homebarH;
}

/* One program: an ITS client handle + its virtualized text view. */
struct Term {
    int             handle = -1;        /* ITS client handle, -1 = not connected */
    lcd_textview_t* tv     = nullptr;   /* the scrollable body (freed with layer) */
    bool            primed = false;     /* Log: first real output replaces hint   */
};

Term s_log;
Term s_cli;
lcd_term_t* s_cliTerm = nullptr;    /* CLI: the libvterm-backed on-device terminal */

/* ---- Log program ---- */

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

void logOnDelete(lv_event_t*) {
    if (s_log.handle >= 0) itsDisconnect(s_log.handle);
    s_log = Term{};        /* the text view frees itself on its container DELETE */
}

void logFn(void* arg) {
    lv_obj_t* layer = (lv_obj_t*)arg;
    s_log.tv = lcdTextViewCreate(layer, lcdScreenW(), termBodyH(),
                                 &lv_font_spleen_5x8, logFg(), scrollbackBudget());
    lcdTextViewSetLineColor(s_log.tv, logLineColor);
    s_log.primed = false;
    lv_obj_add_event_cb(layer, logOnDelete, LV_EVENT_DELETE, nullptr);

    /* {"ansi":0}: plain live lines + plain paste-back (LVGL has no ANSI). */
    static const char proto[] = "{\"ansi\":0}";
    s_log.handle = itsConnect("log", LOG_PORT_DC, proto, sizeof(proto) - 1,
                              pdMS_TO_TICKS(500), 0, logRecvCb, logDiscCb);
    /* Transient hint, replaced by the first line that arrives (logRecvCb). */
    const char* hint = (s_log.handle < 0) ? "[log: connect failed]"
                                          : "(waiting for log output)";
    lcdTextViewSet(s_log.tv, hint, strlen(hint));
}

/* ---- CLI program ----
 * A real terminal (lcd_term / libvterm). The device runs this DC session in
 * CLI_ANSI and echoes + line-edits; an interactive ssh shell drives the screen
 * directly. We forward keystrokes and render whatever comes back — cursor
 * addressing, scrolling and all — so a shell, top, vim and ncurses lay out
 * correctly instead of scrolling off into nowhere. */

/* Bytes the terminal wants to send upstream (keystroke encodings, query
 * replies) → straight to the device CLI session. */
void cliOutput(const char* data, size_t len, void* /*user*/) {
    if (s_cli.handle >= 0) itsSend(s_cli.handle, data, len, pdMS_TO_TICKS(200));
}

void cliRecvCb(int handle, size_t) {
    char tmp[2048];
    size_t n;
    while ((n = itsRecv(handle, tmp, sizeof(tmp), 0)) > 0)
        lcdTermFeed(s_cliTerm, tmp, n);
}

void cliDiscCb(int) {
    s_cli.handle = -1;
    const char* m = "\r\n[cli connection closed]\r\n";
    lcdTermFeed(s_cliTerm, m, strlen(m));
}

void cliOnDelete(lv_event_t*) {
    if (s_cli.handle >= 0) itsDisconnect(s_cli.handle);
    lcdTermDestroy(s_cliTerm);   /* frees the VTerm; LVGL objects go with the layer */
    s_cliTerm = nullptr;
    s_cli = Term{};
}

/* Focus the terminal so the keyboard reaches it. Deferred via a one-shot timer
 * from cliFn because the launcher tile that opened us is focused into the input
 * group on click-release — i.e. AFTER our handlers run — so an immediate focus
 * would be stolen back. The timer fires once that settles; a tap re-focuses. */
void cliFocus(void) {
    lv_obj_t* c = lcdTermObj(s_cliTerm);
    if (c && lv_obj_is_valid(c)) lv_group_focus_obj(c);
}
void cliFocusTimerCb(lv_timer_t*) { cliFocus(); }
void cliFocusClickCb(lv_event_t*) { cliFocus(); }

/* Keys arrive as LV_EVENT_KEY on the focused terminal; libvterm encodes them
 * (incl. arrows / Enter / Backspace) and emits the bytes via cliOutput. */
void cliKeyCb(lv_event_t* e) {
    lcdTermKey(s_cliTerm, lv_event_get_key(e));
}

void cliFn(void* arg) {
    lv_obj_t* layer = (lv_obj_t*)arg;
    s_cliTerm = lcdTermCreate(layer, lcdScreenW(), termBodyH(),
                              &lv_font_spleen_5x8, cliFg(), cliOutput, nullptr);

    lv_obj_t* cont = lcdTermObj(s_cliTerm);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), cont);
    lv_obj_add_event_cb(cont, cliKeyCb,        LV_EVENT_KEY,     nullptr);
    lv_obj_add_event_cb(cont, cliFocusClickCb, LV_EVENT_CLICKED, nullptr);
    lv_timer_t* ft = lv_timer_create(cliFocusTimerCb, 40, nullptr);
    lv_timer_set_repeat_count(ft, 1);
    lcdProgramScrollwheelArrows(true);   /* trackball → arrows while the CLI is up */

    lv_obj_add_event_cb(layer, cliOnDelete, LV_EVENT_DELETE, nullptr);

    /* On-screen key hint, fed locally so it sits in the scrollback ahead of the
     * device's first prompt and scrolls off like any other output. LCD-only —
     * the browser xterm shares CLI_PORT_DC but has a real keyboard. */
    static const char hint[] =
        "Press Alt-C <char> for Ctrl-<char>, trackball does arrows\r\n";
    lcdTermFeed(s_cliTerm, hint, sizeof(hint) - 1);

    /* The device echoes + line-edits in CLI_ANSI; the terminal renders it. The
     * connect payload reports our grid size ("colsxrows") so the device's ssh
     * client can request a correctly-sized pty for ncurses apps. */
    int trows = 0, tcols = 0;
    lcdTermSize(s_cliTerm, &trows, &tcols);
    std::string sz = std::to_string(tcols) + "x" + std::to_string(trows);
    s_cli.handle = itsConnect("cli", CLI_PORT_DC, sz.data(), sz.size(),
                              pdMS_TO_TICKS(500), 0, cliRecvCb, cliDiscCb);
    if (s_cli.handle < 0) {
        const char* m = "[cli: connect failed]\r\n";
        lcdTermFeed(s_cliTerm, m, strlen(m));
    }
}

}  // namespace

void lcdAppsInit(void) {
    lcdLauncherAdd("Log", "log", logFn);
    lcdLauncherAdd("CLI", "cli", cliFn);
}
