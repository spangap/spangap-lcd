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
#include "pm.h"

#include <cstdlib>

/* lcd-task aux ports (must not clash with storage's 1/42/43). */
static constexpr uint16_t LCD_RUN_PORT = 10;
static constexpr uint16_t LCD_REG_PORT = 11;

TaskHandle_t lcdTaskHandle = nullptr;

/* Held while the backlight is on: light sleep would freeze the lcd task (no
 * touch polling, no LVGL refresh) and drop the USB-JTAG console. Released when
 * the backlight is 0 (screen off), so the device can still light-sleep dark. */
static pm_lock_handle_t s_noLightSleep  = nullptr;
static bool             s_lightLockHeld = false;

/* Board HAL, registered by the consumer via lcdSetBoard() before diptychInit(). */
static const lcd_board_t* s_board = nullptr;
void lcdSetBoard(const lcd_board_t* board) { s_board = board; }
const lcd_board_t* lcdBoard(void) { return s_board; }

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
    itsOnAux(LCD_RUN_PORT, onRunMsg);
    itsOnAux(LCD_REG_PORT, onRegMsg);

    if (!lcdLvglInit()) { err("lvgl bring-up failed\n"); killSelf(); }
    lcdIconsInit();

    lv_obj_t* scr = lv_screen_active();
    lcdLauncherInit(scr);                  /* bottom: program icons */
    lcdSettingsInit();                     /* built-in Settings (gear) tile */
    lcdStatusbarInit();                    /* top: opaque status bar (lv_layer_top) */

    /* Keep the device awake while the screen is on (see s_noLightSleep). */
    pmLockCreate(PM_NO_LIGHT_SLEEP, "lcd", &s_noLightSleep);

    /* Live config. Backlight applies on this task; icon_res change reloads the
     * launcher tiles (loader does the flash reads, never us). */
    NOW_AND_ON_CHANGE("s.lcd.backlight", {
        int lvl = atoi(val);
        const lcd_board_t* b = lcdBoard();
        if (b && b->backlight) b->backlight((uint8_t)lvl);
        if (s_noLightSleep) {
            if (lvl > 0 && !s_lightLockHeld)      { pmLockAcquire(s_noLightSleep); s_lightLockHeld = true; }
            else if (lvl == 0 && s_lightLockHeld) { pmLockRelease(s_noLightSleep); s_lightLockHeld = false; }
        }
    });
    storageSubscribeChanges("s.lcd.icon_res", ON_CHANGE {
        if (lcdIconResRefresh()) lcdLauncherReload();
    });

    info("ready (%dx%d)\n", lcdScreenW(), lcdScreenH());

    for (;;) {
        while (itsPoll(0)) {}              /* drain aux / storage callbacks */
        uint32_t idle = lv_timer_handler();
        if (idle == LV_NO_TIMER_READY || idle > 30) idle = 30;
        itsPoll(pdMS_TO_TICKS(idle));      /* wake on next timer or a message */
    }
}

void lcdInit(void) {
    if (lcdTaskHandle) return;

    storageDefault("s.lcd.backlight",    200);
    storageDefault("s.lcd.icon_res",     "64x64");
    storageDefault("s.lcd.date_format",  "%d %b %Y, %H:%M");

    /* PSRAM stack is fine: the lcd task never does flash I/O (the loader does).
     * Core 1 (core 0 hosts WiFi); prio 2. LVGL render needs generous stack. */
    lcdTaskHandle = spawnTask(lcdTaskFn, "lcd", 16384, nullptr, 2, 1, STACK_PSRAM);
    if (!lcdTaskHandle) err("task spawn failed\n");
}
