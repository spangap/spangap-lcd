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
#include "lcd_board.h"
#include "lcd_internal.h"

#include "storage.h"
#include "its.h"
#include "log.h"
#include "compat.h"

#include "esp_attr.h"

#include <cstdlib>

/* lcd-task aux ports (must not clash with storage's 1/42/43). */
static constexpr uint16_t LCD_RUN_PORT = 10;
static constexpr uint16_t LCD_REG_PORT = 11;

TaskHandle_t lcdTaskHandle = nullptr;

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

/* Board HAL, registered by the consumer via lcdSetBoard() before diptychInit(). */
static const lcd_board_t* s_board = nullptr;
void lcdSetBoard(const lcd_board_t* board) { s_board = board; }
const lcd_board_t* lcdBoard(void) { return s_board; }

/* Whether the device has a real text keyboard. The lcd component owns no
 * keyboard; a consumer (e.g. reticulous/main/tdeck.cpp) sets this so Settings text
 * fields edit in place instead of popping the on-screen keyboard. */
static bool s_hasKeyboard = false;
void lcdSetHasKeyboard(bool present) { s_hasKeyboard = present; }
bool lcdHasKeyboard(void) { return s_hasKeyboard; }

/* ---- aux payloads (delivered on the lcd task) ---- */

struct lcd_run_msg_t { lcd_fn_t fn; void* arg; };
struct lcd_reg_msg_t { lcd_fn_t fn; char name[32]; char basename[32]; };
static_assert(sizeof(lcd_run_msg_t) <= ITS_MAX_MSG_DATA, "lcd_run_msg_t too big");
static_assert(sizeof(lcd_reg_msg_t) <= ITS_MAX_MSG_DATA, "lcd_reg_msg_t too big");

static void onRunMsg(TaskHandle_t, const void* d, size_t len) {
    if (len < sizeof(lcd_run_msg_t)) return;
    auto* m = static_cast<const lcd_run_msg_t*>(d);
    if (m->fn) m->fn(m->arg);
}

static void onRegMsg(TaskHandle_t, const void* d, size_t len) {
    if (len < sizeof(lcd_reg_msg_t)) return;
    auto* m = static_cast<const lcd_reg_msg_t*>(d);
    lcdLauncherAdd(m->name, m->basename, m->fn);
}

/* ---- public API ---- */

void lcdRun(lcd_fn_t fn, void* arg) {
    if (!lcdTaskHandle || !fn) return;
    lcd_run_msg_t m{ fn, arg };
    itsSendAuxByTaskHandle(lcdTaskHandle, LCD_RUN_PORT, &m, sizeof(m),
                           pdMS_TO_TICKS(200));
}

void lcdRegister(const char* name, const char* iconBasename, lcd_fn_t fn) {
    if (!lcdTaskHandle || !fn) return;
    lcd_reg_msg_t m{};
    m.fn = fn;
    safeStrncpy(m.name,     name        ? name        : "", sizeof(m.name));
    safeStrncpy(m.basename, iconBasename ? iconBasename : "", sizeof(m.basename));
    itsSendAuxByTaskHandle(lcdTaskHandle, LCD_REG_PORT, &m, sizeof(m),
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

/* ---- task ---- */

static void lcdTaskFn(void*) {
    itsServerInit();                       /* inbox for aux + storage subs */
    itsClientInit(2);                      /* client side: Log + CLI programs dial log:1/cli:1 */
    itsOnAux(LCD_RUN_PORT, onRunMsg);
    itsOnAux(LCD_REG_PORT, onRegMsg);

    if (!lcdLvglInit()) { err("lvgl bring-up failed\n"); killSelf(); }
    lcdIconsInit();

    lv_obj_t* scr = lv_screen_active();
    lcdLauncherInit(scr);                  /* bottom: program icons */
    lcdSettingsInit();                     /* built-in Settings (gear) tile */
    lcdAppsInit();                          /* built-in Log + CLI program tiles */
    lcdStatusbarInit();                    /* top: opaque status bar (lv_layer_top) */

    /* No NO_LIGHT_SLEEP lock: the panel holds its own GRAM, touch wakes the task
     * via a GPIO wake source, and the board keeps the backlight PWM alive across
     * light sleep (board HAL clocks it from RC_FAST with LEDC keep-alive). The
     * device can light-sleep with the screen on. */

    /* Live config. Backlight applies on this task; icon_res change reloads the
     * launcher tiles (loader does the flash reads, never us). */
    NOW_AND_ON_CHANGE("s.lcd.backlight", {
        int lvl = atoi(val);
        const lcd_board_t* b = lcdBoard();
        if (b && b->backlight) b->backlight((uint8_t)lvl);
    });
    storageSubscribeChanges("s.lcd.icon_res", ON_CHANGE {
        if (lcdIconResRefresh()) lcdLauncherReload();
    });

    info("ready (%dx%d)\n", lcdScreenW(), lcdScreenH());

    for (;;) {
        while (itsPoll(0)) {}              /* drain aux / storage callbacks */
        if (s_inputPending) {             /* woke on a touch/button/key edge */
            s_inputPending = false;
            while (lcdInputPoll()) {}      /* drain click / keystroke synthesis */
        }
        /* LVGL resumes a pointer indev's read timer on press (for its own
         * long-press timing) and only pauses it on release — if that pause is
         * missed, LVGL auto-reads the pointer at ~30 Hz forever (repositioning the
         * cursor → redraws → quiescent CPU). diptych drives all input manually and
         * doesn't use LVGL's input timers, so enforce it: pause any released
         * indev's read timer. */
        lcdPauseIdleInputTimers();
        /* Event-mode indevs mean lv_timer_handler no longer polls input, so its
         * idle return is honest: sleep until the next LVGL timer (animation,
         * clock, touch-tracking) or an ISR/ITS wake — no more 30ms input poll. */
        uint32_t idle = lv_timer_handler();
        itsPoll(idle == LV_NO_TIMER_READY ? portMAX_DELAY : pdMS_TO_TICKS(idle));
    }
}

void lcdInit(void) {
    if (lcdTaskHandle) return;

    storageDefault("s.lcd.backlight",    200);
    storageDefault("s.lcd.icon_res",     "40x40");
    storageDefault("s.lcd.date_format",  "%d %b %Y, %H:%M");

    /* PSRAM stack is fine: the lcd task never does flash I/O (the loader does).
     * Core 1 (core 0 hosts WiFi); prio 2. LVGL render needs generous stack. */
    lcdTaskHandle = spawnTask(lcdTaskFn, "lcd", 16384, nullptr, 2, 1, STACK_PSRAM);
    if (!lcdTaskHandle) err("task spawn failed\n");
}
