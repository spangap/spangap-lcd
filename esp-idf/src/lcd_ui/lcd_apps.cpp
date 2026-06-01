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
 * Plain text: both connect to the packet-mode DC ports. Log connects with
 * {"ansi":0} so it gets paste-back + live lines WITHOUT ANSI colour escapes
 * (LVGL can't render them); CLI uses LINE mode (the device echoes nothing, we
 * echo locally). Scrollback is capped to s.log.file.paste kB — the same knob
 * that sizes the log paste-back.
 */
#include "lcd_internal.h"

#include "its.h"
#include "log.h"
#include "cli.h"
#include "storage.h"

#include <string>
#include <cstring>

namespace {

lv_color_t logFg() { return lv_color_hex(0xC8C8C8); }   /* light grey */
lv_color_t cliFg() { return lv_color_hex(0xC8E8C8); }   /* pale green */

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
std::string s_cliEdit;              /* CLI: the command line being typed (inline) */
int         s_cliEsc  = 0;          /* CLI output escape-filter state (see cliFeed)  */
std::string s_cliOsc;               /* accumulated OSC body, for the 5379 raw toggle  */
bool        s_cliRaw  = false;      /* raw passthrough (interactive ssh): no local echo */

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
 * Inline terminal: no separate input box. The command line being typed lives in
 * s_cliEdit and is rendered as the text view's transient suffix (committed
 * output sits in the scrollback, which ends in the server's "$ " prompt),
 * followed by a cursor. The container is the keyboard focus target; cliKeyCb is
 * the line editor. The device CLI's DC port is LINE mode — we echo locally. */

/* Push the in-progress line + cursor as the transient suffix. When the user is
 * typing (pin), pull the view down to the live prompt regardless of scroll pos. */
void cliShowEdit(bool pin) {
    if (!s_cli.tv) return;
    std::string suf = s_cliEdit;
    suf.push_back('_');                             /* cursor */
    lcdTextViewSetSuffix(s_cli.tv, suf.data(), suf.size());
    if (pin) lcdTextViewScrollToBottom(s_cli.tv);
}

/* Enter/leave raw passthrough. In raw mode the remote pty owns echo + editing
 * (interactive ssh shell): cliKeyCb sends keystrokes verbatim and we show no
 * local edit line. Toggled by our private OSC 5379 (see ssh_client.cpp / the
 * browser's TerminalWindow.vue). */
void cliSetRaw(bool on) {
    if (s_cliRaw == on) return;
    s_cliRaw = on;
    s_cliEdit.clear();
    if (s_cli.tv) lcdTextViewSetSuffix(s_cli.tv, on ? "" : "_", on ? 0 : 1);
}

/* Feed device→screen bytes through an escape-sequence filter before they reach
 * the text view, which is plain-text only (no cursor addressing / colour). CSI,
 * OSC and other escapes are dropped; printable text + \n \r \t \b pass through.
 * Our own OSC 5379;1/;0 is recognised and flips raw mode. State persists across
 * calls — a sequence can straddle an itsRecv boundary. */
void cliFeed(const char* data, size_t n) {
    if (!s_cli.tv) return;
    char out[2048];
    size_t o = 0;
    auto flush = [&]() { if (o) { lcdTextViewAppend(s_cli.tv, out, o); o = 0; } };
    auto oscDone = [&]() {
        if      (s_cliOsc == "5379;1") cliSetRaw(true);
        else if (s_cliOsc == "5379;0") cliSetRaw(false);
        s_cliOsc.clear();
    };
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (s_cliEsc) {
        case 0:                                          /* normal text */
            if (c == 0x1b) s_cliEsc = 1;                 /* ESC — start of a sequence */
            else if (c >= 0x20 || c == '\n' || c == '\r' || c == '\t' || c == '\b') {
                if (o == sizeof(out)) flush();
                out[o++] = (char)c;
            }                                            /* other control bytes: drop */
            break;
        case 1:                                          /* just saw ESC */
            if (c == '[')                       s_cliEsc = 2;                 /* CSI */
            else if (c == ']')                  { s_cliEsc = 3; s_cliOsc.clear(); }  /* OSC */
            else if (c=='('||c==')'||c=='*'||c=='+')  s_cliEsc = 5;           /* charset: eat 1 */
            else if (c=='P'||c=='X'||c=='^'||c=='_')  { s_cliEsc = 3; s_cliOsc.clear(); } /* DCS/SOS/PM/APC: to ST */
            else                                s_cliEsc = 0;                 /* 2-byte esc: done */
            break;
        case 2:                                          /* CSI: params… then a final 0x40–0x7e */
            if (c >= 0x40 && c <= 0x7e) s_cliEsc = 0;
            break;
        case 3:                                          /* OSC/string: until BEL or ST (ESC \) */
            if (c == 0x07)      { oscDone(); s_cliEsc = 0; }
            else if (c == 0x1b) s_cliEsc = 4;
            else if (s_cliOsc.size() < 32) s_cliOsc.push_back((char)c);
            break;
        case 4:                                          /* OSC saw ESC, expect '\' (ST) */
            oscDone();
            s_cliEsc = (c == '\\') ? 0 : (c == 0x1b ? 4 : 0);
            break;
        case 5:                                          /* charset designator: drop one byte */
            s_cliEsc = 0;
            break;
        }
    }
    flush();
}

void cliRecvCb(int handle, size_t) {
    char tmp[2048];
    size_t n;
    while ((n = itsRecv(handle, tmp, sizeof(tmp), 0)) > 0)
        cliFeed(tmp, n);
}

void cliDiscCb(int) {
    s_cli.handle = -1;
    if (s_cli.tv) {
        const char* m = "\n[cli connection closed]\n";
        lcdTextViewAppend(s_cli.tv, m, strlen(m));
    }
}

void cliOnDelete(lv_event_t*) {
    if (s_cli.handle >= 0) itsDisconnect(s_cli.handle);
    s_cli = Term{};        /* the text view frees itself on its container DELETE */
    s_cliEdit.clear();
    s_cliEsc = 0; s_cliOsc.clear(); s_cliRaw = false;   /* fresh filter/mode on reopen */
}

/* Focus the terminal so the keyboard reaches it. Deferred via a one-shot timer
 * from cliFn because the launcher tile that opened us is focused into the input
 * group on click-release — i.e. AFTER our handlers run — so an immediate focus
 * would be stolen back. The timer fires once that settles; a tap re-focuses. */
void cliFocus(void) {
    lv_obj_t* c = lcdTextViewObj(s_cli.tv);
    if (c && lv_obj_is_valid(c)) lv_group_focus_obj(c);
    dbg("cliFocus: cont=%p now-focused=%p\n", (void*)c,
        (void*)(lcdInputGroup() ? lv_group_get_focused(lcdInputGroup()) : nullptr));   /* TEMP probe */
}
void cliFocusTimerCb(lv_timer_t*) { cliFocus(); }
void cliFocusClickCb(lv_event_t*) { cliFocus(); }

/* Inline line editor — keys arrive as LV_EVENT_KEY on the focused terminal. */
void cliKeyCb(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (s_cliRaw) {
        /* Raw passthrough (interactive ssh): one byte per key, no local echo or
         * editing — the remote pty echoes through cliFeed. */
        char b;
        if      (key == LV_KEY_ENTER)     b = '\r';
        else if (key == LV_KEY_BACKSPACE) b = 0x7f;
        else if (key == LV_KEY_ESC)       b = 0x1b;
        else if (key >= 0x20 && key < 0x7F) b = (char)key;
        else return;                                    /* nav/other — nothing to send */
        if (s_cli.handle >= 0) itsSend(s_cli.handle, &b, 1, pdMS_TO_TICKS(200));
        return;
    }
    if (key == LV_KEY_ENTER) {
        std::string line = s_cliEdit;
        line.push_back('\n');
        if (s_cli.tv) lcdTextViewAppend(s_cli.tv, line.data(), line.size());  /* echo */
        if (s_cli.handle >= 0)
            itsSend(s_cli.handle, line.data(), line.size(), pdMS_TO_TICKS(200));
        s_cliEdit.clear();
    } else if (key == LV_KEY_BACKSPACE) {
        if (!s_cliEdit.empty()) s_cliEdit.pop_back();
    } else if (key == LV_KEY_ESC) {
        s_cliEdit.clear();
    } else if (key >= 0x20 && key < 0x7F) {
        s_cliEdit.push_back((char)key);
    } else {
        return;                                     /* nav/other — leave as-is */
    }
    cliShowEdit(true);                              /* typing pins to the prompt */
}

void cliFn(void* arg) {
    lv_obj_t* layer = (lv_obj_t*)arg;
    s_cli.tv = lcdTextViewCreate(layer, lcdScreenW(), termBodyH(),
                                 &lv_font_spleen_5x8, cliFg(), scrollbackBudget());
    s_cliEdit.clear();

    /* The scrollable terminal is the keyboard focus target + line editor. */
    lv_obj_t* cont = lcdTextViewObj(s_cli.tv);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), cont);
    lv_obj_add_event_cb(cont, cliKeyCb,        LV_EVENT_KEY,     nullptr);
    lv_obj_add_event_cb(cont, cliFocusClickCb, LV_EVENT_CLICKED, nullptr);
    lv_timer_t* ft = lv_timer_create(cliFocusTimerCb, 40, nullptr);
    lv_timer_set_repeat_count(ft, 1);

    lv_obj_add_event_cb(layer, cliOnDelete, LV_EVENT_DELETE, nullptr);

    /* DC port is LINE mode (device echoes nothing — we echo locally). */
    s_cli.handle = itsConnect("cli", CLI_PORT_DC, nullptr, 0,
                              pdMS_TO_TICKS(500), 0, cliRecvCb, cliDiscCb);
    if (s_cli.handle < 0) {
        const char* m = "[cli: connect failed]\n";
        lcdTextViewSet(s_cli.tv, m, strlen(m));
    }
    cliShowEdit(false);            /* show the cursor; don't force-scroll on open */
}

}  // namespace

void lcdAppsInit(void) {
    lcdLauncherAdd("Log", "log", logFn);
    lcdLauncherAdd("CLI", "cli", cliFn);
}
