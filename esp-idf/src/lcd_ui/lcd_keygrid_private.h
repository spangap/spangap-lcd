/**
 * lcd_keygrid_private.h — the key grid's own data (see lcd_keygrid.h).
 *
 * Forked from LVGL v9's lv_buttonmatrix_private.h (MIT). lcd_keys.c needs the
 * struct because a keyboard IS a key grid with a state machine on top, exactly
 * as upstream's keyboard extends its button matrix.
 */

#ifndef LCD_KEYGRID_PRIVATE_H
#define LCD_KEYGRID_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "core/lv_obj_private.h"
#include "lcd_keygrid.h"

/**********************
 *      TYPEDEFS
 **********************/

/** Data of the key grid */
struct _lcd_keygrid_t {
    lv_obj_t obj;
    const char * const * map_p;          /**< Pointer to the current map */
    const char * const * mark_tl;        /**< Per-button top-left mark, or NULL. Not owned; see lcd_keygrid_set_marks */
    const char * const * mark_tr;        /**< Per-button top-right mark, or NULL */
    lv_area_t * button_areas;            /**< Array of areas of buttons */
    lcd_keygrid_ctrl_t * ctrl_bits;      /**< Array of control bytes */
    uint32_t btn_cnt;                    /**< Number of button in 'map_p'(Handled by the library) */
    uint32_t row_cnt;                    /**< Number of rows in 'map_p'(Handled by the library) */
    uint32_t btn_id_sel;                 /**< Index of the active button (being pressed/released etc) or LCD_KEYGRID_BUTTON_NONE */
    uint32_t one_check : 1;              /**< 1: Single button toggled at once */
    uint32_t auto_free_map : 1;          /**< 1: Automatically free the map when the widget is deleted */
    uint32_t pressed : 1;                /**< A finger is down on btn_id_sel. OURS, not the object's
                                          *  LV_STATE_PRESSED: see lcd_keygrid_event */
};

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LCD_KEYGRID_PRIVATE_H*/
