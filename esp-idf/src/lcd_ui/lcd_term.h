/**
 * lcd_term.h — a real VT100/xterm terminal for the on-device screen, backed by
 * vendored libvterm. Unlike lcdTextView (append-only scrollback), this is a
 * fixed rows×cols cell grid with cursor addressing, so full-screen apps (top,
 * vim, anything ncurses) lay out correctly over ssh.
 *
 * The owner feeds device→screen bytes with lcdTermFeed() and delivers focused
 * key events with lcdTermKey(); bytes the terminal needs to send upstream
 * (keystroke encodings, query replies) come back through the output callback.
 * Rendering is one LVGL label per row, repainted only on the rows libvterm
 * marks damaged.
 */
#pragma once

#include "lvgl.h"
#include <cstddef>
#include <cstdint>

struct lcd_term_t;

/** Bytes the terminal wants to send upstream (to the device CLI / remote pty).
 *  Invoked on the lcd task from within lcdTermKey()/lcdTermFeed(). */
typedef void (*lcd_term_output_cb)(const char* data, size_t len, void* user);

/** Create a terminal filling w×h of `parent`. rows/cols are derived from the
 *  (monospace) font metrics. The returned container is the keyboard focus
 *  target — see lcdTermObj(). */
lcd_term_t* lcdTermCreate(lv_obj_t* parent, int32_t w, int32_t h,
                          const lv_font_t* font, lv_color_t fg,
                          lcd_term_output_cb onOutput, void* user);

/** Feed device→screen bytes (the VT byte stream). Repaints damaged rows. */
void        lcdTermFeed(lcd_term_t* t, const char* data, size_t len);

/** Deliver one focused key (an LVGL key code or a printable ASCII value).
 *  Encoded by libvterm and emitted via the output callback. */
void        lcdTermKey(lcd_term_t* t, uint32_t lvKey);

/** Current grid size (for an ssh pty-req / window-change). */
void        lcdTermSize(lcd_term_t* t, int* rows, int* cols);

/** The LVGL object to join to the input group / hang click handlers on. */
lv_obj_t*   lcdTermObj(lcd_term_t* t);

/** Free the VTerm. The LVGL objects are freed with their parent layer. */
void        lcdTermDestroy(lcd_term_t* t);
