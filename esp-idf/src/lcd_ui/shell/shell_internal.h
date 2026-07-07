/**
 * shell_internal.h — cross-file glue for the phone shell (src/lcd_ui/shell + apps).
 *
 * Only compiled when CONFIG_LCD_PHONE=y. Public app API is lcd_app.h; the
 * retained display/input glue is lcd_internal.h. Everything here runs on the lcd
 * task. The shell provides the full legacy public surface (lcd.h: lcdRegister,
 * lcdShowProgram, lcdGoHome, lcdProgram*, lcdScroll, lcdAtLauncher, lcdLauncherAdd
 * for onRegMsg, the statusbar entry points) so unconverted straddles keep linking
 * while apps migrate to LcdApp one at a time.
 */
#pragma once

#include "lvgl.h"
#include "lcd_app.h"

#include <vector>

/* ---- shell.cpp / manager.cpp: the state machine ---- */
/** Build the whole shell into `screen` (status bar, launcher, nav) and install
 *  the built-in apps. Called from lcd.cpp's task bring-up instead of the legacy
 *  lcdLauncherInit/lcdSettingsInit/lcdAppsInit/lcdStatusbarInit quartet. */
void shellInit(lv_obj_t* screen);

/** The single navigation consumer. Producers (gesture, ESC, Home button, nav
 *  bar) all funnel here. */
void shellNavigate(NavIntent intent);

/** Bring `app` to the foreground (lazy onCreate, then onShow, raise, restore
 *  focus). The launcher tile click and lcdShowProgram() call this. */
void shellOpenApp(LcdApp* app);

/** The app currently in the foreground, or nullptr at the launcher/recents. */
LcdApp* shellForeground(void);

/** Look up an installed app by its Config::name (the lcdShowProgram key). */
LcdApp* shellFindApp(const char* name);

/** The screen the manager is showing. */
enum class ShellScreen { LAUNCHER, APP, RECENTS };
ShellScreen shellScreen(void);

/** Re-apply chrome (status bar visibility, layer geometry, trackball-arrows)
 *  for `app` — a no-op unless it is the foreground app. Called when an app
 *  flips one of its UI-state flags (setFullscreen / setScrollwheelArrows). */
void shellAppChanged(LcdApp* app);

/** Evict an app: if foreground, go Home first; then onClose(), free the ledger,
 *  delete the root layer (next open rebuilds). Drops it from the running set. */
void shellEvictApp(LcdApp* app);

/** Every installed app (lcd_app.cpp). The "running set" (recents) is the subset
 *  whose root() is non-null. */
const std::vector<LcdApp*>& shellApps(void);

/* ---- nav.cpp: producers ---- */
/** Install the top-level ESC->Back key hook (tracks the focused object via the
 *  input group). Called from shellInit. */
void shellNavInstall(void);

/* ---- recents.cpp: the app switcher (M3) ---- */
/** Show / hide the recents screen (cards for the running set + memory label). */
void shellRecentsShow(void);
void shellRecentsHide(void);
bool shellRecentsVisible(void);

/* ---- launcher.cpp: paged icon grid ---- */
/** Build the paged launcher into `screen` (called by shellInit). */
void shellLauncherInit(lv_obj_t* screen);
/** Add a tile for an installed app on its Config::launcherPage. */
void shellLauncherAddTile(LcdApp* app);
/** The launcher's root container (a lower sibling of program layers). */
lv_obj_t* shellLauncherRoot(void);
/** Tear down + rebuild the launcher at the current UI scale (tile geometry +
 *  fonts + icon sizes reflow). Called on a runtime zoom change. Lcd task. */
void shellLauncherRebuild(void);
/** Recalibrate + reflow the whole shell for a new UI zoom (s.lcd.scale): new
 *  font sizes, launcher grid, statusbar. Lcd task. */
void shellApplyZoom(void);

/* ---- statusbar.cpp: renderer + setters (lcd_internal.h declares the
 *      lcdStatusbarInit / lcdStatusbarSetVisible entry points the shell reuses) ---- */

/* ---- built-in app factories (apps/) ---- */
/** The on-device Log program as an LcdApp (apps/log_app.cpp). */
LcdApp* lcdMakeLogApp(void);
/** The on-device CLI program as an LcdApp (apps/cli_app.cpp). */
LcdApp* lcdMakeCliApp(void);

/* ---- nav / chrome: the home-bar gesture lives with the manager in M1; M3
 *      promotes it to nav.cpp with the RECENTS dwell + ESC + nav bar. ---- */
