/**
 * lcd — phone-style LVGL launcher + display owner.
 *
 * Owns the display + input devices and the one LVGL context. LVGL is not
 * thread-safe, so everything that touches it runs on the lcd task: use
 * lcdRun()/ON_LCD to hop onto it (mirrors storage's ON_CHANGE) — that is the
 * only place LVGL may be touched.
 *
 * Layering (bottom -> top): launcher (program icons) | per-program layers |
 * opaque status bar. A component registers a program with lcdRegister(); when
 * its icon is opened, lcd creates (or re-shows) that program's own layer and
 * calls the registered fn with the layer as its argument. The fn must touch
 * nothing outside that layer. A swipe up from the bottom edge (or lcdGoHome())
 * hides the layer back to the launcher; layers persist, so re-opening resumes
 * where it left off.
 *
 * Gated on CONFIG_DIPTYCH_LCD. lcdInit() is called by diptychInit(); the
 * consumer's board layer must implement the lcd_board.h contract.
 */
#ifndef DIPTYCH_LCD_H
#define DIPTYCH_LCD_H

#include "lvgl.h"

/** Callback run on the lcd task. `arg` is reserved for lcdRun() (defaults
 *  null) and is the program's layer object (lv_obj_t*) for lcdRegister(). */
typedef void (*lcd_fn_t)(void* arg);

/** Lambda -> lcd_fn_t sugar, mirroring storage's ON_CHANGE. No captures. */
#define ON_LCD [](void* arg)

/** Bring up display, LVGL, launcher and status bar, and spawn the lcd task.
 *  Called by diptychInit() when CONFIG_DIPTYCH_LCD=y. Safe to call once. */
void lcdInit(void);

/** Run fn(arg) on the lcd task — the only place LVGL may be touched. Returns
 *  once queued; fn runs shortly after on the lcd task. Safe from any task. */
void lcdRun(lcd_fn_t fn, void* arg = nullptr);

/** Register a launcher program. `name` is the icon label; `iconBasename`
 *  names a file under /fixed/lcd/icons/<res>/<iconBasename>.bin (no extension,
 *  no size — <res> is fixed to the launcher tile size, "36x36"; icons render at
 *  that native size). `fn` runs on the lcd task with
 *  the program's layer (lv_obj_t*) the first time the icon is opened, or again
 *  if the layer was reclaimed. Safe to call from any task / any time. */
void lcdRegister(const char* name, const char* iconBasename, lcd_fn_t fn);

/** Set backlight 0..255 (0 = off). Persists s.lcd.backlight. Any task. */
void lcdSetBacklight(uint8_t level);

/** Hide the current program layer and return to the launcher. Runs on the lcd
 *  task; safe to call from a registered fn (e.g. a Back button). */
void lcdGoHome(void);

/** The shared keypad focus group that hardware button / keyboard indevs target.
 *  Add focusable widgets you build in a program (e.g. a textarea) with
 *  lv_group_add_obj() so a physical keyboard can type into them. Lcd task. */
lv_group_t* lcdInputGroup(void);

/** Tell the lcd component whether the device has a real text keyboard. The lcd
 *  component owns no keyboard itself (a consumer may provide one as its own
 *  keypad indev joined to lcdInputGroup()); call this so the built-in Settings
 *  text fields edit in place instead of popping an on-screen keyboard. Default
 *  false (no keyboard → on-screen keyboard). Any task. */
void lcdSetHasKeyboard(bool present);

/** How long the cursor (driven by a board pointer device) stays visible after
 *  activity, in milliseconds; <0 = always visible. lcd owns the cursor but not
 *  this policy — the consumer that owns the pointing device sets it (e.g. from
 *  its own config key). Runs on / hops to the lcd task. */
void lcdPointerSetVisibleMs(int ms);

/* ---- Settings ---- */

/** Register an entry in the built-in Settings menu (the gear program lcd owns).
 *  `path` is a slash-path whose intermediate segments become submenus
 *  (auto-created, title-cased), e.g. "net/wifi" -> a "Net" submenu with a
 *  "Wi-Fi" item. `label` names the leaf item; `fn` is called on the lcd task
 *  with the item's content widget (an empty, scrollable flex-column container —
 *  build the pane into it with the lcdSetting* helpers). Call at init, before
 *  Settings is first opened; populates a registry, so it works even before
 *  lcdInit() and from any init task. */
void lcdRegisterSettings(const char* path, const char* label, lcd_fn_t fn);

/* ---- Setting-pane helpers ----
 * Build a uniform labeled row in `parent` (a Settings pane widget) bound to a
 * storage key, mirroring the browser's Setting* components. Run on the lcd
 * task (inside a settings fn). Each returns the row object. */

/** Bold section divider (no control). */
lv_obj_t* lcdSettingSection (lv_obj_t* parent, const char* title);
/** Toggle bound to an int key (0/1). */
lv_obj_t* lcdSettingSwitch  (lv_obj_t* parent, const char* label, const char* key);
/** Slider bound to an int key, clamped to [min,max]. */
lv_obj_t* lcdSettingSlider  (lv_obj_t* parent, const char* label, const char* key, int min, int max);
/** Text field bound to a string key; tap opens an on-screen keyboard. When
 *  `secret`, the value is masked. */
lv_obj_t* lcdSettingText    (lv_obj_t* parent, const char* label, const char* key, bool secret = false);
/** Dropdown bound to a string key; `optionsCsv` is a comma-separated list. */
lv_obj_t* lcdSettingDropdown(lv_obj_t* parent, const char* label, const char* key, const char* optionsCsv);
/** Read-only row that live-updates from a string key (auto-refreshes ~1 Hz). */
lv_obj_t* lcdSettingValue   (lv_obj_t* parent, const char* label, const char* key);
/** Action button; `onClick` runs on the lcd task (arg is the row). */
lv_obj_t* lcdSettingButton  (lv_obj_t* parent, const char* label, lcd_fn_t onClick);

#endif /* DIPTYCH_LCD_H */
