/**
 * lcd.cpp — lcd task + public API (lcdRun / lcdRegister / lcdSetBacklight /
 * lcdGoHome) and config defaults.
 *
 * The lcd task owns the one LVGL context. It is an itsPoll task: lcdRun /
 * lcdRegister hand work onto it as ITS aux messages (mirroring how storage
 * delivers ON_CHANGE), and storage change subscriptions are dispatched here
 * too. Because itsPoll's notification accounting must stay intact, this task
 * NEVER does flash I/O — icon files are read by the loader task in
 * lcd_icons.cpp and the bytes handed back via lcdRun.
 */
#include "lcd.h"
#include "lcd_input.h"
#include "lcd_internal.h"
#include "shell_internal.h"   /* shellInit — the phone-style shell */

#include "storage.h"
#include "its.h"
#include "log.h"
#include "compat.h"

#include "esp_attr.h"
#include "freertos/semphr.h"

#include <cstdlib>

/* lcd-task aux port (must not clash with storage's 1/42/43). */
static constexpr uint16_t LCD_RUN_PORT = 10;

TaskHandle_t lcdTaskHandle = nullptr;

/* Handshake: lcdInit() blocks on this until the lcd task has the panel + first
 * frame up. The panel shares the SPI bus with other peripherals whose init
 * runs after lcdInit() returns (e.g. a radio's), so the bring-up — the one-shot
 * controller init sequence especially — must finish before the dispatcher
 * moves on, or those inits race it on the bus. Given by the task once, then
 * dropped by lcdInit(). */
static SemaphoreHandle_t s_bringupDone = nullptr;

/* Set by lcdInputISR (any input INT), cleared by the loop after it reads the
 * indevs. The notify wakes itsPoll; this flag tells the loop *why* it woke so it
 * only touches the input hardware on a real edge — never on a timer tick. */
static volatile bool s_inputPending = false;

/* The one input ISR, shared by every input INT line (the board attaches it to
 * touch / button / keyboard). It only flags + wakes us — all reads happen on the
 * lcd task. IRAM_ATTR + DRAM-only: the gpio ISR service is installed with
 * ESP_INTR_FLAG_IRAM (LoRa's DIO1 path), so this may not reach flash/PSRAM. */
extern "C" void IRAM_ATTR lcdInputISR(void* /*arg*/) {
    s_inputPending = true;
    if (!lcdTaskHandle) return;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(lcdTaskHandle, &hp);
    portYIELD_FROM_ISR(hp);
}

/* Same flag + wake as lcdInputISR, from task context (no FromISR variant). */
extern "C" void lcdInputSignal(void) {
    s_inputPending = true;
    if (lcdTaskHandle) xTaskNotifyGive(lcdTaskHandle);
}

/* Input HAL, registered by the consumer via lcdSetInput() before spangapInit().
 * The display itself is owned by the lcd component (lcd_panel.cpp, Kconfig). */
static const lcd_input_t* s_input = nullptr;
void lcdSetInput(const lcd_input_t* in) { s_input = in; }
const lcd_input_t* lcdInput(void) { return s_input; }

/* Whether the device has a real text keyboard. The lcd component owns no
 * keyboard; a consumer (e.g. reticulous/main/tdeck.cpp) sets this so Settings text
 * fields edit in place instead of popping the on-screen keyboard. */
static bool s_hasKeyboard = false;
void lcdSetHasKeyboard(bool present) { s_hasKeyboard = present; }
bool lcdHasKeyboard(void) { return s_hasKeyboard; }

/* ---- aux payloads (delivered on the lcd task) ---- */

struct lcd_run_msg_t { lcd_fn_t fn; void* arg; };
static_assert(sizeof(lcd_run_msg_t) <= ITS_MAX_MSG_DATA, "lcd_run_msg_t too big");

static void onRunMsg(TaskHandle_t, const void* d, size_t len) {
    if (len < sizeof(lcd_run_msg_t)) return;
    auto* m = static_cast<const lcd_run_msg_t*>(d);
    if (m->fn) m->fn(m->arg);
}

/* ---- public API ---- */

void lcdRun(lcd_fn_t fn, void* arg) {
    if (!lcdTaskHandle || !fn) return;
    lcd_run_msg_t m{ fn, arg };
    itsSendAuxByTaskHandle(lcdTaskHandle, LCD_RUN_PORT, &m, sizeof(m),
                           pdMS_TO_TICKS(200));
}

void lcdSetBacklight(uint8_t level) {
    /* Single source of truth: the s.lcd.backlight subscription (below) applies
     * it on the lcd task. storageSet fires that subscription. */
    storageSet("s.lcd.backlight", (int)level);
}

void lcdGoHome(void) {
    lcdRun([](void*) { lcdGoHomeInternal(); });
}

bool lcdNotifyActivity(void) {
    /* On the lcd task (e.g. a consumer keyboard read_cb): act inline and report
     * whether it woke the screen, so the caller can swallow the waking key.
     * Off-task: post it and report no wake (only on-task input needs the result). */
    if (xTaskGetCurrentTaskHandle() == lcdTaskHandle) return lcdActivity();
    lcdRun([](void*) { lcdActivity(); });
    return false;
}

/* ---- task ---- */

static void lcdTaskFn(void*) {
    itsServerInit();                       /* inbox for aux + storage subs */
    itsClientInit(2);                      /* client side: Log + CLI programs dial log:1/cli:1 */
    itsOnAux(LCD_RUN_PORT, onRunMsg);

    if (!lcdLvglInit()) {
        err("lvgl bring-up failed\n");
        if (s_bringupDone) xSemaphoreGive(s_bringupDone);   /* unblock lcdInit() */
        killSelf();
    }
    lcdFontsInit();      /* bring up the font engine before the shell resolves fonts */
    lcdIconsInit();

    lv_obj_t* scr = lv_screen_active();
    /* The phone-style shell: status bar + paged launcher + LcdApp lifecycle.
     * Installs the built-in Settings/Log/CLI apps; other straddles install their
     * programs from their *LcdRegister hooks via lcdRun(lcdInstall(...)). */
    shellInit(scr);

    /* No NO_LIGHT_SLEEP lock: the panel holds its own GRAM, touch wakes the task
     * via a GPIO wake source, and the board keeps the backlight PWM alive across
     * light sleep (board HAL clocks it from RC_FAST with LEDC keep-alive). The
     * device can light-sleep with the screen on. */

    /* Live config. Backlight target applies on this task (held dark until the boot
     * reveal). Icons are rasterized on demand at the tile size (no icon_res
     * bucket); a zoom change reloads the launcher via lcdStyleRecalibrate. */
    NOW_AND_ON_CHANGE("s.lcd.backlight", { lcdBacklightSetTarget((uint8_t)atoi(val)); });
    /* UI zoom: recalibrate fonts + reflow the launcher/statusbar on change (the
     * initial value is already applied at lcdStyleBegin, so subscribe-only). */
    storageSubscribeChanges("s.lcd.scale", ON_CHANGE { shellApplyZoom(); });
    /* Inactivity: after s.lcd.inactivity_timeout s with no input we set the
     * ephemeral sys.standby key; the board decides what standby means (and clears
     * the key to wake). 0 = never. */
    NOW_AND_ON_CHANGE("s.lcd.inactivity_timeout", { lcdInactivitySetTimeout(atoi(val)); });

    info("ready (%dx%d)\n", lcdScreenW(), lcdScreenH());
    /* Release lcdInit(): the panel controller is initialised and the UI tree is
     * built, so the bring-up's one-shot SPI sequence is off the bus before the
     * dispatcher starts other shared-bus peripherals. The first frame is
     * flushed by the loop's lv_timer_handler() just below — NOT with an inline
     * lv_refr_now() here, which would flush off the loop path and desync the
     * itsPoll notification accounting (loop then spins in the aux-drain and
     * never renders or reads input). */
    if (s_bringupDone) xSemaphoreGive(s_bringupDone);

    /* Hold a manual boost to 240 MHz while any LVGL animation is running. Anim
     * frames come as timer wakes (not notifies), so the notify model would leave
     * the inertial fling / transitions at the DFS floor; this keeps them smooth.
     * Released the instant the last animation ends. */
    bool animBoosted = false;
    for (;;) {
        while (itsPoll(0)) {}              /* drain aux / storage callbacks */
        if (s_inputPending) {             /* woke on a touch/button/trackball edge */
            s_inputPending = false;
            lcdActivity();                 /* re-arm the inactivity timer */
            /* Drain click / keystroke. A press while in standby is handled by the
             * board: its click_read clears sys.standby (waking us) and absorbs the
             * press, so it never lands as a click / go-home / cursor move on wake. */
            while (lcdInputPoll()) {}
        }
        /* While blanked, skip rendering entirely and sleep until the next input
         * edge or ITS message — no 1 Hz status-clock wake, so the chip can
         * light-sleep. (Keyboard keys arrive as lcdRun aux and wake us here.) */
        if (lcdScreenIsOff()) {
            if (animBoosted) { pmBoostEnd(); animBoosted = false; }   /* never pin 240 while blanked */
            itsPoll(portMAX_DELAY); continue;
        }
        /* LVGL resumes a pointer indev's read timer on press (for its own
         * long-press timing) and only pauses it on release — if that pause is
         * missed, LVGL auto-reads the pointer at ~30 Hz forever (repositioning the
         * cursor → redraws → quiescent CPU). spangap drives all input manually and
         * doesn't use LVGL's input timers, so enforce it: pause any released
         * indev's read timer. */
        lcdPauseIdleInputTimers();
        /* Event-mode indevs mean lv_timer_handler no longer polls input, so its
         * idle return is honest: sleep until the next LVGL timer (animation,
         * clock, touch-tracking) or an ISR/ITS wake — no more 30ms input poll. */
        uint32_t idle = lv_timer_handler();
        /* Boost across active animations (their frames are timer wakes, not
         * notifies). Acquire on the rising edge, release when the last one ends. */
        bool anim = lv_anim_count_running() > 0;
        if (anim && !animBoosted)      { pmBoost();    animBoosted = true; }
        else if (!anim && animBoosted) { pmBoostEnd(); animBoosted = false; }
        itsPoll(idle == LV_NO_TIMER_READY ? portMAX_DELAY : pdMS_TO_TICKS(idle));
    }
}

void lcdInit(void) {
    if (lcdTaskHandle) return;

    storageBegin();
    storageDefault("s.lcd.backlight",    200);
    storageDefault("s.lcd.scale",        100);   /* UI zoom %, clamp 50–200 */
    storageDefault("s.lcd.date_format",  "%d %b %Y, %H:%M");
    storageDefault("s.lcd.inactivity_timeout", 30);   /* s; 0 = never blank */
    storageEnd();

    /* PSRAM stack is fine: the lcd task never does flash I/O (the loader does).
     * Core 1 (core 0 hosts WiFi); prio 2. LVGL render needs generous stack —
     * the deepest paths (nanosvg icon raster + vector-font glyph rasterization,
     * plus a 2 KB scratch buffer in the CLI/log apps) overflowed the former
     * 16 KB intermittently ("stack overflow in task lcd" at boot). A PSRAM stack
     * overflow scribbles into adjacent PSRAM — a silent corruptor — so give it
     * real headroom; PSRAM is plentiful. Watch the high-water mark via `top`. */
    s_bringupDone = xSemaphoreCreateBinary();
    /* Priority 5 on core 1 for a snappy UI: the lcd task preempts the ordinary
     * prio-1 actors instead of round-robining with them, so rendering and input
     * never wait behind storage/web/cli work. This is safe now that storage's
     * heavy tasks (actor, save/deflate, notify) run on core 0 — the original
     * "long UI computation starves the core-1 actors and wedges the device"
     * hazard was about the storage actor above all, and it's no longer here.
     * The remaining core-1 prio-1 tasks (web, cli, log, fs) do yield to the UI;
     * that's the point. Renders are now incremental (reconciled bubbles, one
     * conversation), so there's no unbounded compute to hold the CPU. */
    lcdTaskHandle = spawnTask(lcdTaskFn, "lcd", 32768, nullptr, 5, 1, STACK_PSRAM);
    if (!lcdTaskHandle) {
        err("task spawn failed\n");
        if (s_bringupDone) { vSemaphoreDelete(s_bringupDone); s_bringupDone = nullptr; }
        return;
    }

    /* Block here until the task signals the panel is up (or a bounded timeout,
     * so a bring-up fault can't wedge boot). On return the panel owns the bus
     * cleanly and the dispatcher can bring up the other shared-SPI peripherals. */
    if (s_bringupDone) {
        if (xSemaphoreTake(s_bringupDone, pdMS_TO_TICKS(5000)) != pdTRUE)
            warn("panel bring-up did not signal in 5 s; continuing\n");
        vSemaphoreDelete(s_bringupDone);
        s_bringupDone = nullptr;
    }
}
