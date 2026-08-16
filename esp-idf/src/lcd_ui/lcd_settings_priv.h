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

/** Two-column row: a 1/3 right-aligned label column and a 2/3 control area. */
lv_obj_t* lcdSettingsMakeRow(lv_obj_t* parent);
/** Add the label column's text to a row from lcdSettingsMakeRow(). */
void      lcdSettingsRowLabel(lv_obj_t* row, const char* text);
/** Stretch a control across the row's remaining 2/3. */
void      lcdSettingsFillControl(lv_obj_t* w);
/** Set a read-only value label: an em-dash when empty, bullets when secret. */
void      lcdSettingsValueText(lv_obj_t* lbl, const char* v, bool secret);

/** Take down every descriptor-runtime modal (open form, dialogs, on-screen
 *  keyboard) and drop their subscriptions. The Settings app calls this as it
 *  closes: the modals live on lv_layer_top, outside the app's widget tree, so
 *  the app's own teardown never reaches them. */
void      lcdSettingsDescReset(void);
