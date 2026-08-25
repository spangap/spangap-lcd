/**
 * lcd_settings_priv.h — the row scaffolding lcd_settings.cpp owns, shared with
 * the descriptor runtime in lcd_settings_desc.cpp.
 *
 * A form field and an item-editor row must be indistinguishable from the pane
 * rows the lcdSetting* helpers build, so the descriptor runtime reaches for the
 * same primitives instead of growing a parallel set that would drift. Private
 * to src/lcd_ui; lcd task only.
 */
#pragma once

#include "lvgl.h"
#include "lcd_settings_desc.h"

#include <cstddef>

/** Two-column row: a 1/3 right-aligned label column and a 2/3 control area.
 *  `compact` gives it three quarters of the pane's row height — for the rows
 *  inside a modal, where the whole point is fitting more of them on screen. */
lv_obj_t* lcdSettingsMakeRow(lv_obj_t* parent, bool compact = false);

/** Halve a control's vertical padding. Every button and input field in Settings
 *  is built through this: the theme sizes them for a finger on a tablet, and on
 *  a pane of stacked rows that verticality is what pushes content off the
 *  bottom. Halves what the widget already has rather than imposing a number, so
 *  it keeps the theme's proportions. */
void      lcdSettingsHalfPadVer(lv_obj_t* w);
/** Add the label column's text to a row from lcdSettingsMakeRow(). */
void      lcdSettingsRowLabel(lv_obj_t* row, const char* text);
/** Stretch a control across the row's remaining 2/3. */
void      lcdSettingsFillControl(lv_obj_t* w);
/** Set a read-only value label: an em-dash when empty, bullets when secret. */
void      lcdSettingsValueText(lv_obj_t* lbl, const char* v, bool secret);

/* ---- integer rows ----
 *
 * The arithmetic behind an `integer:` row, shared so a pane row and the same
 * row inside a form agree about what a number means. A bound stated as a key
 * (lcd_num_t::min_key / max_key) is resolved here, so callers read a bound
 * rather than a description of one. */

/** The effective lower/upper bound: the device-published one where the
 *  descriptor names a key for it, otherwise the compiled number. Only
 *  meaningful where the matching `has_min` / `has_max` is set. */
int  lcdSettingsNumMin(const lcd_num_t* spec);
int  lcdSettingsNumMax(const lcd_num_t* spec);
/** `cur` stepped one press in `dir` (+1 / -1) and clamped to the bounds. The
 *  step SNAPS to its own multiples: at step 5, down from 23 is 20 and then 15. */
int  lcdSettingsNumStep(const lcd_num_t* spec, int cur, int dir);
/** Is `v` inside the stated bounds? */
bool lcdSettingsNumOk(const lcd_num_t* spec, int v);
/** The sentence shown when it is not — "Enter a number between 0 and 300." */
void lcdSettingsNumRangeMsg(const lcd_num_t* spec, char* buf, size_t n);
/** Parse a typed number. False on empty or on anything that is not one, which
 *  is what keeps a half-typed "-" out of storage. */
bool lcdSettingsNumParse(const char* text, int* out);
/** Modal with one line of text and an OK: how a refused number says so. */
void lcdSettingsNotice(const char* text);

/** Is this a dotted quad — four octets of 0-255? EMPTY IS TRUE: an address
 *  field left blank is unset, which is how a fixed IP is handed back to DHCP. */
bool lcdSettingsIpv4Ok(const char* text);

/** Hand the keypad focus ring back to `opener` when `overlay` is deleted.
 *
 *  Every full-screen overlay adds its own widgets to the one global input
 *  group, at the tail. LVGL refocuses to the PREVIOUS group member when the
 *  focused object is deleted (lv_group_create defaults to
 *  LV_GROUP_REFOCUS_POLICY_PREV), so closing a modal lands the ring on
 *  whatever widget was created last before it opened — on a settings pane that
 *  is its BOTTOM row, which scrolls itself into view and, being a text row on a
 *  keyboard board, blinks a caret there. Focus belongs where it was taken from.
 *  Registered on LV_EVENT_DELETE, which LVGL sends to an object before deleting
 *  its children, so by the time the overlay's own widgets go there is nothing
 *  focused left to refocus. */
void      lcdSettingsRefocusOnClose(lv_obj_t* overlay, lv_obj_t* opener);

/** Cover the screen with "Restarting the device." — for a setting whose change
 *  the device reboots to apply. The panel goes with the reboot, so there is
 *  nothing to wait for and nothing to take it down again. */
void      lcdSettingsRebootNotice(void);

/** Take down every descriptor-runtime modal (open form, dialogs, on-screen
 *  keyboard) and drop their subscriptions. The Settings app calls this as it
 *  closes: the modals live on lv_layer_top, outside the app's widget tree, so
 *  the app's own teardown never reaches them. */
void      lcdSettingsDescReset(void);
