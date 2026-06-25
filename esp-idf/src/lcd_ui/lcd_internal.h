/**
 * lcd_internal.h — cross-file glue for the lcd component (private to src/lcd_ui).
 *
 * Public API is lcd.h; the input HAL is lcd_input.h. This header wires the
 * internal modules: core task (lcd.cpp), panel bring-up (lcd_panel.cpp), LVGL
 * bring-up (lcd_lvgl.cpp), icon cache + loader (lcd_icons.cpp), launcher
 * (lcd_launcher.cpp), status bar (lcd_statusbar.cpp). Everything here runs on
 * the lcd task unless noted.
 */
#pragma once

#include "lvgl.h"
#include "lcd.h"
#include "lcd_input.h"
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Opaque status bar height (px), reserved at the top of the screen. Program
 * layers and the launcher live below it. */
#define LCD_STATUSBAR_H 24

/* ---- lcd.cpp ---- */
/** The lcd task handle (for aux sends from other tasks / the loader). */
extern TaskHandle_t lcdTaskHandle;
/** Registered input HAL (lcdSetInput), or nullptr if none. */
const lcd_input_t* lcdInput(void);
/** Whether a consumer reported a hardware text keyboard (lcdSetHasKeyboard).
 *  Settings text fields edit in place when true, else use the on-screen kb. */
bool lcdHasKeyboard(void);

/* ---- lcd_panel.cpp: generic Kconfig-driven panel + backlight ---- */
/** Bring up the SPI bus + panel-io + controller from CONFIG_LCD_*; returns the
 *  panel handle and, via the out params, the panel-IO handle and the final
 *  (post-rotation) display size. Installs the shared GPIO ISR service. nullptr
 *  on failure. */
esp_lcd_panel_handle_t lcdPanelInit(esp_lcd_panel_io_handle_t* ioOut, int* wOut, int* hOut);
/** Backlight 0..255 (0 = off) via CONFIG_LCD_BL_PIN (LEDC). No-op if no BL pin. */
void        lcdPanelBacklight(uint8_t level);
/** Panel display on/off for the inactivity standby (GRAM retained → instant wake). */
void        lcdPanelDisplayPower(bool on);
/** Map a raw native touch point to display coordinates using CONFIG_LCD_ROTATION
 *  + mirror (the same transform applied to the pixels); out params are clamped. */
void        lcdPanelOrientTouch(int rawX, int rawY, int* outX, int* outY);

/* ---- lcd_lvgl.cpp: display + input bring-up ---- */
/** lcdPanelInit + lv_init + display/flush/tick + (optional) touch/pointer/button
 *  indevs. Returns false on failure. */
bool        lcdLvglInit(void);
int         lcdScreenW(void);
int         lcdScreenH(void);
/** Read every event-mode indev once (touch / button / keyboard / pointer).
 *  Called from the lcd loop when an input ISR has flagged a pending edge.
 *  Returns true if a read callback wants an immediate follow-up read (mid-click
 *  / keystroke drain), so the caller re-polls before sleeping. */
bool        lcdInputPoll(void);
/** Pause any released indev's LVGL read timer (LVGL resumes it on press for its
 *  own timing and can miss the release-pause, leaving it auto-reading at ~30 Hz).
 *  Called each lcd-loop pass; keeps the task idle when nothing is held. */
void        lcdPauseIdleInputTimers(void);

/* ---- inactivity timeout / standby / backlight (lcd_lvgl.cpp; lcd task only) ----
 * lcdScreenSleep/lcdScreenWake are the standby primitives — declared in lcd.h, as
 * the board (not the lcd component) drives them off the sys.standby key. */
/** Set the inactivity timeout (seconds; <=0 = never) and (re)arm the timer. On
 *  expiry the lcd component sets the ephemeral sys.standby key; the board acts on
 *  it. Driven by the s.lcd.inactivity_timeout subscription. */
void        lcdInactivitySetTimeout(int seconds);
/** Register user input: re-arms the inactivity timer. Always returns false (waking
 *  from standby is the board's job, via sys.standby); kept bool for callers. */
bool        lcdActivity(void);
/** True while the screen is in standby (lcdScreenSleep). The lcd loop skips
 *  rendering and sleeps until input while this holds. */
bool        lcdScreenIsOff(void);
/** Set the backlight target (s.lcd.backlight). Applied at once while awake and past
 *  the boot reveal; otherwise remembered for the next fade-in. */
void        lcdBacklightSetTarget(uint8_t level);
/** Nudge the one-shot boot reveal: the backlight is held dark from boot and fades
 *  up once launcher icon loads go quiet (or a hard cap fires). Called as icons land. */
void        lcdBootSettleKick(void);
/** Focus group for non-pointer indevs (encoder/keypad). Launcher icons join
 *  it so a trackball-only board navigates the same UI. */
lv_group_t* lcdInputGroup(void);

/** Set trackball→arrows mode (the launcher calls this as the shown layer
 *  changes; see lcdProgramScrollwheelArrows). Hides the pointer while on. */
void        lcdScrollwheelArrowsApply(bool on);

/* ---- lcd_icons.cpp: RAM cache + lv_fs driver + loader ---- */
/** Register the in-RAM lv_fs driver ('D') and start the loader task. Call
 *  after lv_init(). */
void        lcdIconsInit(void);
/** Resolve a basename to its LVGL src path at the (fixed) launcher resolution,
 *  e.g. "D:/fixed/lcd/icons/36x36/rns.bin". Returns out. */
const char* lcdIconSrc(const char* basename, char* out, size_t outLen);
/** True iff the current-resolution bytes for `basename` are already cached
 *  (lcd-task-only). */
bool        lcdIconReady(const char* basename);
/** Ask the loader (off the lcd task) to fetch the current-resolution bytes
 *  for `basename`. On completion the lcd task caches them and calls
 *  lcdLauncherIconLoaded(basename). No-op if already cached. */
void        lcdIconRequest(const char* basename);
/** The launcher's fixed icon resolution bucket (LAUNCHER_ICON_RES, "36x36"). */
const char* lcdIconRes(void);
/** No-op now the resolution is fixed; always returns false. Kept for the
 *  s.lcd.icon_res subscription wiring in lcd.cpp. */
bool        lcdIconResRefresh(void);

/* ---- lcd_launcher.cpp ---- */
void        lcdLauncherInit(lv_obj_t* screen);
/** Add (or refresh) a program tile, with an optional per-show callback. Runs on
 *  the lcd task. */
void        lcdLauncherAdd(const char* name, const char* basename, lcd_fn_t fn,
                           lcd_fn_t showFn = nullptr);
/** A basename's bytes just landed in the cache — set the real image. */
void        lcdLauncherIconLoaded(const char* basename);
/** Re-resolve every tile's icon src after an icon_res change. */
void        lcdLauncherReload(void);
/** Hide the current program layer and reveal the launcher. */
void        lcdGoHomeInternal(void);

/* ---- lcd_statusbar.cpp ---- */
void        lcdStatusbarInit(void);
/** Show/hide the opaque top status bar (e.g. for an immersive program screen).
 *  The launcher coordinates this with the current layer's geometry — programs
 *  should call lcdProgramFullscreen(), not this, to reclaim the bar's space. */
void        lcdStatusbarSetVisible(bool visible);

/* ---- lcd_settings.cpp ---- */
/** Register the built-in Settings (gear) program with the launcher. */
void        lcdSettingsInit(void);

/* ---- lcd_apps.cpp: built-in Log + CLI programs ---- */
/** Register the built-in Log + CLI programs. Call from the lcd task after the
 *  launcher exists; needs itsClientInit() (the lcd task is an ITS client). */
void        lcdAppsInit(void);
/** Spleen 5x8 monospace bitmap font (generated lv_font_spleen_5x8.c). */
extern const lv_font_t lv_font_spleen_5x8;
/** Tom Thumb 4x6 monospace bitmap font (generated lv_font_tomthumb_4x6.c). */
extern const lv_font_t lv_font_tomthumb_4x6;
/** Micro 2x3 thumbnail font (generated lv_font_micro_2x3.c). */
extern const lv_font_t lv_font_micro_2x3;
