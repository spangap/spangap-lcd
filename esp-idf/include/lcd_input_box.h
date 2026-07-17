#pragma once
/**
 * lcd_input_box.h — a device-friendly text entry, a thin wrapper over
 * lv_textarea. Generic (no consumer specifics): any program that needs a text
 * field gets the same behaviours a phone user expects on a keyboard-and-trackball
 * device:
 *
 *   • Auto-grows between minLines and maxLines as text wraps / newlines are typed;
 *     scrolls internally past maxLines.
 *   • Double-space within 300 ms collapses to ". " (fast sentence breaks).
 *   • lcdInputBoxText() returns the body with trailing whitespace stripped.
 *   • "Caret-active" edit state: a click or keystroke lights the caret; while lit,
 *     a relative-pointing HAL (trackball) drives the caret instead of the pointer
 *     (see lcdCaretActive / lcdCaretRelease in lcd.h). Keyboard focus is meant to
 *     rest on the box permanently — a keystroke re-lights the caret even after a
 *     walk-out, so typing never depends on where focus wandered.
 *   • submit-on-Enter is configurable: on, Enter fires LV_EVENT_READY (and inserts
 *     no newline); off, Enter inserts a newline like any multi-line field.
 *
 * Lcd task only (like the rest of the UI).
 */
#include "lvgl.h"

/** Create the entry as a child of `parent`. Returns the lv_textarea, so the
 *  caller styles it (font/colours), joins it to lcdInputGroup(), and adds its own
 *  LV_EVENT_READY / LV_EVENT_VALUE_CHANGED handlers. minLines/maxLines bound the
 *  auto-grow (e.g. 1..4). */
lv_obj_t* lcdInputBoxCreate(lv_obj_t* parent, int minLines, int maxLines);

/** The current body with trailing whitespace removed (what should actually be
 *  transmitted). Valid until the next call on the same box; copy to keep. */
const char* lcdInputBoxText(lv_obj_t* box);

/** Whether Enter submits (fires LV_EVENT_READY, no newline inserted) or inserts a
 *  newline. Default on. A consumer toggles this to offer a "compose multi-line"
 *  mode. */
void lcdInputBoxSetSubmitOnEnter(lv_obj_t* box, bool on);

/** Change the auto-grow bounds at runtime (e.g. a Signal-style expand toggle
 *  flipping between a 1–4 line field and a fixed 8-line one via min==max). Resizes
 *  the box immediately. */
void lcdInputBoxSetLines(lv_obj_t* box, int minLines, int maxLines);

/** Focus the box (if not already) and light its caret — for a tap anywhere in a
 *  surrounding compose area, so the whole field is a focus target, not just its
 *  text. Cheap and scroll-free when the box already holds focus. */
void lcdInputBoxActivate(lv_obj_t* box);
