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
    uint8_t popovers : 1;       /**< Show button titles in popovers on press */
};

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LCD_KEYS_PRIVATE_H*/
