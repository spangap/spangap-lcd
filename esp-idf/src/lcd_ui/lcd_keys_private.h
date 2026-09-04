/**
 * lcd_keys_private.h — the keyboard widget's own data (see lcd_keys.h).
 *
 * Forked from LVGL v9's lv_keyboard_private.h (MIT). A keyboard IS a key grid
 * with a mode and a bound field, so the struct opens with the grid's.
 */

#ifndef LCD_KEYS_PRIVATE_H
#define LCD_KEYS_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "lcd_keygrid_private.h"
#include "lcd_keys.h"

/**********************
 *      TYPEDEFS
 **********************/

/** Where the shift key stands. ONCE is spent by the next key, whatever it is;
 *  LOCK is only left by pressing shift again. Pressing shift walks
 *  OFF → ONCE → LOCK → OFF, so the double press an operator makes to lock is
 *  simply two steps of that walk and needs no timer. */
typedef enum {
    LCD_KEYS_SHIFT_OFF,
    LCD_KEYS_SHIFT_ONCE,
    LCD_KEYS_SHIFT_LOCK,
} lcd_keys_shift_t;

/** What a flick off a key came to mean. UP is the one direction anybody can
 *  aim at without looking, so it gets the mark people reach for most. */
typedef enum {
    LCD_KEYS_FLICK_NONE,
    LCD_KEYS_FLICK_SHIFT,   /**< thrown upwards: the top-right mark */
    LCD_KEYS_FLICK_FN,      /**< thrown any other way: the top-left mark */
} lcd_keys_flick_t;

/** Most cells the long-press chooser will show: the key's own character, its two
 *  modifier alternates, and the accented forms after them. O is the widest key
 *  in the table — o, O, its fn backslash and eight alternates, eleven — and the
 *  twelfth is the room to add one. Twelve fits a 320 px panel as two rows of
 *  six at the size these are drawn; the cells narrow to fit before the count is
 *  ever cut. */
#define LCD_KEYS_ALT_MAX 12

/** No cell is chosen. The chooser opens in this state and stays in it until the
 *  finger is actually on a character, so a hold that meant nothing types
 *  nothing. */
#define LCD_KEYS_ALT_NONE 0xFF

/** One cell of the chooser: the UTF-8 it types (a single character, so five
 *  bytes covers any of them) and the label drawn for it. */
typedef struct {
    char       txt[5];
    lv_obj_t * label;
} lcd_keys_alt_t;

/** Data of the keyboard */
struct _lcd_keys_t {
    lcd_keygrid_t btnm;
    lv_obj_t * ta;              /**< Pointer to the assigned text area */
    lv_obj_t * sink;            /**< Assigned key sink: keys go here as LV_EVENT_KEY instead */
    lcd_keys_mode_t mode;       /**< Key map type */
    uint32_t last_tick;         /**< When the last key went down, for the double tap */
    uint32_t last_btn;          /**< …and which one it was */
    uint8_t shift : 2;          /**< Sticky shift state (lcd_keys_shift_t) */
    uint8_t fn : 1;             /**< fn is armed for the next key. Never latches */
    uint8_t ctrl : 1;           /**< ctrl is armed for the next key. Never latches */
    uint8_t popovers : 1;       /**< Show button titles in popovers on press */

    /* ---- the flick (see the block over lcd_keys_flick_cb in lcd_keys.c) ----
     * A key can be pressed, or it can be thrown: a short drag off a key reaches
     * one of its corner marks without arming a modifier first. */
    uint8_t flick : 2;          /**< lcd_keys_flick_t: which mark the throw meant */
    uint32_t flick_btn;         /**< the key it started on, kept because the grid
                                 *   has been made to forget it by then */
    int32_t flick_x, flick_y;   /**< where the finger landed */
    int32_t flick_min;          /**< how far it must travel, in px, worked out
                                 *   from the pressed key's own height */

    /* ---- the long-press chooser (see the block over lcd_keys_alt_open) ----
     * Held down, a key opens a row of everything else it can produce; the
     * finger slides along it and lifts on one. */
    lv_obj_t * alt_pop;         /**< the panel, on the top layer, or NULL */
    lcd_keys_alt_t alt[LCD_KEYS_ALT_MAX];
    const lv_font_t * alt_font; /**< the face its characters are drawn in, which
                                 *   also sets the cell size (lcd_keys_set_alt_font) */
    uint8_t alt_cnt;            /**< cells in use */
    uint8_t alt_sel;            /**< the cell the finger is on, or LCD_KEYS_ALT_NONE */
};

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LCD_KEYS_PRIVATE_H*/
