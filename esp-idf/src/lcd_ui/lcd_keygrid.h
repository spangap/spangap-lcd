/**
 * lcd_keygrid.h — a grid of keys: OUR FORK of LVGL's lv_buttonmatrix.
 *
 * Copied from LVGL v9 (MIT, https://github.com/lvgl/lvgl) and renamed, so that
 * the on-screen keyboard can grow what the upstream widget will not give us:
 * per-key markings (the alternate a long press produces, printed small in the
 * key's corner), a long-press chooser, per-key hit margins and per-key
 * styling. Nothing else in the tree draws a key grid, so this is the keyboard's
 * own widget in all but name.
 *
 * Kept deliberately close to upstream for now — same structure, same internal
 * names (`btn_*`, `btnm`), so an upstream fix stays easy to read across and
 * apply. Ours diverges by intent, one feature at a time; when a divergence
 * lands, say so here.
 *
 * Divergences so far:
 *   • renamed (lv_buttonmatrix → lcd_keygrid, LV_BUTTONMATRIX → LCD_KEYGRID)
 *     and lifted out of LVGL's Kconfig gates — it is always built with us
 *   • include paths are the component-external form (LVGL exports its root and
 *     src/ as include dirs)
 *
 *   • per-key corner marks (lcd_keygrid_set_marks): two small legends in a
 *     button's top corners, and the button's own legend moved down and left to
 *     make room. This is the keyboard's shift/fn alternates, printed where a
 *     physical keycap prints them.
 *
 *   • three control bits upstream left reserved or free now mean something:
 *     ACCENT draws a button in LV_STATE_USER_1 (a latch — caps lock), STRUCK
 *     rules a diagonal line across its legend (a keyboard glyph crossed out),
 *     PAD draws a three-by-four grid of dots in place of one (the key that
 *     leads to the numeric pad, wearing a picture of it)
 *
 *   • a press marks OUR `pressed` bit instead of the object's LV_STATE_PRESSED,
 *     because that state change invalidates the whole widget — half a panel of
 *     SPI to colour one key. Two key rectangles now, and nothing else.
 *
 * NO THEME KNOWS THIS CLASS. LVGL's default theme dresses widgets by comparing
 * class pointers, so a grid built straight from here draws with no key
 * background and no legend colour — laid out and hit-tested perfectly, and
 * invisible. Style LV_PART_ITEMS yourself; lcd_keys does exactly that for the
 * keyboard (lcd_keys_apply_look).
 */

#ifndef LCD_KEYGRID_H
#define LCD_KEYGRID_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lvgl.h"
#include "core/lv_obj.h"
#include "core/lv_obj_property.h"

/*********************
 *      DEFINES
 *********************/
#define LCD_KEYGRID_BUTTON_NONE 0xFFFF
LV_EXPORT_CONST_INT(LCD_KEYGRID_BUTTON_NONE);

/**********************
 *      TYPEDEFS
 **********************/

/* Upstream keeps every widget's struct typedef in LVGL's own lv_types.h; ours
 * are declared with the widget, which is where a fork's belong. */
typedef struct _lcd_keygrid_t lcd_keygrid_t;

/** Type to store button control flags (disabled, hidden etc.)
 *  The least-significant 4 bits are used to store button-width proportions in range [1..15]. */
typedef enum {
    LCD_KEYGRID_CTRL_NONE         = 0x0000, /**< No extra control, use the default settings*/
    LCD_KEYGRID_CTRL_WIDTH_1      = 0x0001, /**< Set the width to 1 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_2      = 0x0002, /**< Set the width to 2 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_3      = 0x0003, /**< Set the width to 3 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_4      = 0x0004, /**< Set the width to 4 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_5      = 0x0005, /**< Set the width to 5 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_6      = 0x0006, /**< Set the width to 6 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_7      = 0x0007, /**< Set the width to 7 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_8      = 0x0008, /**< Set the width to 8 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_9      = 0x0009, /**< Set the width to 9 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_10     = 0x000A, /**< Set the width to 10 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_11     = 0x000B, /**< Set the width to 11 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_12     = 0x000C, /**< Set the width to 12 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_13     = 0x000D, /**< Set the width to 13 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_14     = 0x000E, /**< Set the width to 14 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_WIDTH_15     = 0x000F, /**< Set the width to 15 relative to the other buttons in the same row */
    LCD_KEYGRID_CTRL_HIDDEN       = 0x0010, /**< Hides button; it continues to hold its space in layout. */
    LCD_KEYGRID_CTRL_NO_REPEAT    = 0x0020, /**< Do not emit LV_EVENT_LONG_PRESSED_REPEAT events while button is long-pressed. */
    LCD_KEYGRID_CTRL_DISABLED     = 0x0040, /**< Disables button like LV_STATE_DISABLED on normal Widgets. */
    LCD_KEYGRID_CTRL_CHECKABLE    = 0x0080, /**< Enable toggling of LV_STATE_CHECKED when clicked. */
    LCD_KEYGRID_CTRL_CHECKED      = 0x0100, /**< Make the button checked. It will use the :cpp:enumerator:`LV_STATE_CHECHKED` styles. */
    LCD_KEYGRID_CTRL_CLICK_TRIG   = 0x0200, /**< 1: Enables sending LV_EVENT_VALUE_CHANGE on CLICK, 0: sends LV_EVENT_VALUE_CHANGE on PRESS. */
    LCD_KEYGRID_CTRL_POPOVER      = 0x0400, /**< Show button text in a pop-over while being pressed. */
    LCD_KEYGRID_CTRL_RECOLOR      = 0x0800, /**< Enable text recoloring with `#color` */
    LCD_KEYGRID_CTRL_ACCENT       = 0x1000, /**< Draw the button in :cpp:enumerator:`LV_STATE_USER_1` — a latch that is neither pressed nor merely checked. */
    LCD_KEYGRID_CTRL_STRUCK       = 0x2000, /**< Rule a diagonal line across the button's legend, in the LV_PART_ITEMS line style. */
    LCD_KEYGRID_CTRL_PAD          = 0x4000, /**< Draw a 3x4 grid of dots centred in the button, in the legend's colour — a key that leads to a numeric pad, wearing a picture of one. The cell's own text still draws, so give such a key a blank legend. */
    LCD_KEYGRID_CTRL_CUSTOM_1     = 0x8000, /**< Custom free-to-use flag */
} lcd_keygrid_ctrl_t;

typedef bool (*lcd_keygrid_button_draw_cb_t)(lv_obj_t * btnm, uint32_t btn_id, const lv_area_t * draw_area,
                                                 const lv_area_t * clip_area);

LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t lcd_keygrid_class;


/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Create a button matrix object
 * @param parent    pointer to an object, it will be the parent of the new button matrix
 * @return          pointer to the created button matrix
 */
lv_obj_t * lcd_keygrid_create(lv_obj_t * parent);

/*=====================
 * Setter functions
 *====================*/

/**
 * Set a new map. Buttons will be created/deleted according to the map. The
 * button matrix keeps a reference to the map and so the string array must not
 * be deallocated during the life of the matrix.
 * @param obj       pointer to a button matrix object
 * @param map       pointer a string array. The last string has to be: "". Use "\n" to make a line break.
 */
void lcd_keygrid_set_map(lv_obj_t * obj, const char * const map[]);

/**
 * Set the button control map (hidden, disabled etc.) for a button matrix.
 * The control map array will be copied and so may be deallocated after this
 * function returns.
 * @param obj       pointer to a button matrix object
 * @param ctrl_map  pointer to an array of `lv_button_ctrl_t` control bytes. The
 *                  length of the array and position of the elements must match
 *                  the number and order of the individual buttons (i.e. excludes
 *                  newline entries).
 *                  An element of the map should look like e.g.:
 *                 `ctrl_map[0] = width | LCD_KEYGRID_CTRL_NO_REPEAT |  LCD_KEYGRID_CTRL_TGL_ENABLE`
 */
void lcd_keygrid_set_ctrl_map(lv_obj_t * obj, const lcd_keygrid_ctrl_t ctrl_map[]);

/**
 * Set the marks printed small in the top corners of the buttons — on a keyboard,
 * what a modifier makes the key produce. A button with a mark on either side is
 * a CAP: its own legend moves down and left, out from under them. A NULL entry
 * means no mark, and a button with neither is centred as usual; "" is a cap with
 * that corner empty.
 *
 * The arrays are NOT copied — they must outlive the widget — and they are
 * dropped by `lcd_keygrid_set_map()`, since a new map is a new set of buttons.
 * Set the map first, then the marks.
 *
 * Top-left marks draw in the LV_PART_CUSTOM_FIRST text style, top-right in
 * LV_PART_INDICATOR, so a caller styles the two independently of the legend
 * (LV_PART_ITEMS).
 *
 * @param obj       pointer to a button matrix object
 * @param marks_tl  top-left mark per button, or NULL for none at all
 * @param marks_tr  top-right mark per button, or NULL for none at all
 */
void lcd_keygrid_set_marks(lv_obj_t * obj, const char * const marks_tl[], const char * const marks_tr[]);

/**
 * Set the selected buttons
 * @param obj        pointer to button matrix object
 * @param btn_id     0 based index of the button to modify. (Not counting new lines)
 */
void lcd_keygrid_set_selected_button(lv_obj_t * obj, uint32_t btn_id);

/**
 * Set the attributes of a button of the button matrix
 * @param obj       pointer to button matrix object
 * @param btn_id    0 based index of the button to modify. (Not counting new lines)
 * @param ctrl      OR-ed attributes. E.g. `LCD_KEYGRID_CTRL_NO_REPEAT | LCD_KEYGRID_CTRL_CHECKABLE`
 */
void lcd_keygrid_set_button_ctrl(lv_obj_t * obj, uint32_t btn_id, lcd_keygrid_ctrl_t ctrl);

/**
 * Clear the attributes of a button of the button matrix
 * @param obj       pointer to button matrix object
 * @param btn_id    0 based index of the button to modify. (Not counting new lines)
 * @param ctrl      OR-ed attributes. E.g. `LCD_KEYGRID_CTRL_NO_REPEAT | LCD_KEYGRID_CTRL_CHECKABLE`
 */
void lcd_keygrid_clear_button_ctrl(lv_obj_t * obj, uint32_t btn_id, lcd_keygrid_ctrl_t ctrl);

/**
 * Set attributes of all buttons of a button matrix
 * @param obj       pointer to a button matrix object
 * @param ctrl      attribute(s) to set from `lcd_keygrid_ctrl_t`. Values can be ORed.
 */
void lcd_keygrid_set_button_ctrl_all(lv_obj_t * obj, lcd_keygrid_ctrl_t ctrl);

/**
 * Clear the attributes of all buttons of a button matrix
 * @param obj       pointer to a button matrix object
 * @param ctrl      attribute(s) to set from `lcd_keygrid_ctrl_t`. Values can be ORed.
 */
void lcd_keygrid_clear_button_ctrl_all(lv_obj_t * obj, lcd_keygrid_ctrl_t ctrl);

/**
 * Set a single button's relative width.
 * This method will cause the matrix be regenerated and is a relatively
 * expensive operation. It is recommended that initial width be specified using
 * `lcd_keygrid_set_ctrl_map` and this method only be used for dynamic changes.
 * @param obj       pointer to button matrix object
 * @param btn_id    0 based index of the button to modify.
 * @param width     relative width compared to the buttons in the same row. [1..15]
 */
void lcd_keygrid_set_button_width(lv_obj_t * obj, uint32_t btn_id, uint32_t width);

/**
 * Make the button matrix like a selector widget (only one button may be checked at a time).
 * `LCD_KEYGRID_CTRL_CHECKABLE` must be enabled on the buttons to be selected using
 * `lcd_keygrid_set_ctrl()` or `lcd_keygrid_set_button_ctrl_all()`.
 * @param obj       pointer to a button matrix object
 * @param en        whether "one check" mode is enabled
 */
void lcd_keygrid_set_one_checked(lv_obj_t * obj, bool en);

/*=====================
 * Getter functions
 *====================*/

/**
 * Get the current map of a button matrix
 * @param obj       pointer to a button matrix object
 * @return          the current map
 */
const char * const * lcd_keygrid_get_map(const lv_obj_t * obj);

/**
 * Get the index of the lastly "activated" button by the user (pressed, released, focused etc)
 * Useful in the `event_cb` to get the text of the button, check if hidden etc.
 * @param obj       pointer to button matrix object
 * @return          index of the last released button (LCD_KEYGRID_BUTTON_NONE: if unset)
 */
uint32_t lcd_keygrid_get_selected_button(const lv_obj_t * obj);

/**
 * Get the button's text
 * @param obj       pointer to button matrix object
 * @param btn_id    the index a button not counting new line characters.
 * @return          text of btn_index` button
 */
const char * lcd_keygrid_get_button_text(const lv_obj_t * obj, uint32_t btn_id);

/**
 * Get the whether a control value is enabled or disabled for button of a button matrix
 * @param obj       pointer to a button matrix object
 * @param btn_id    the index of a button not counting new line characters.
 * @param ctrl      control values to check (ORed value can be used)
 * @return          true: the control attribute is enabled false: disabled
 */
bool lcd_keygrid_has_button_ctrl(lv_obj_t * obj, uint32_t btn_id, lcd_keygrid_ctrl_t ctrl);

/**
 * Tell whether "one check" mode is enabled or not.
 * @param obj       Button matrix object
 * @return          true: "one check" mode is enabled; false: disabled
 */
bool lcd_keygrid_get_one_checked(const lv_obj_t * obj);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LCD_KEYGRID_H*/
