/**
 * lcd_keys.h — the on-screen keyboard WIDGET: OUR FORK of LVGL's lv_keyboard.
 *
 * Copied from LVGL v9 (MIT) alongside lcd_keygrid (its lv_buttonmatrix), which
 * it is built on. Upstream's keyboard is a key grid plus four fixed layouts and
 * a handler that types into a textarea; ours is where the layouts we actually
 * want live — the number row and the modifier alternates are in, and still to
 * come are the per-field layouts (hex for an address, a URL row) and the
 * long-press chooser for accented letters.
 *
 * THE TEXT LAYOUT, five rows staggered like a typewriter's. The Q row fills the
 * panel and sets the key; the rows under it are that same key stepped right —
 * A three tenths of one past Q, Z half a one past A:
 *
 *     |1|2|3|4|5|6|7|8|9|0|⌫|
 *     |Q |W |E |R |T |Y |U |I |O |P |
 *      ␣|A |S |D |F |G |H |J |K |L |  ␣
 *        ␣|Z |X |C |V |B |N |M |, |. |␣
 *     |shift| ⌨̸ | fn |  space  |  ⏎  |
 *
 * ONE MAP, AND THE CAPS NEVER CHANGE. They are printed the way a physical
 * keyboard prints them — upper case, with what shift makes small in the
 * top-right corner and what fn makes small in the top-left, in blue — so a
 * modifier changes what a key PRODUCES and never what it says. Nothing shifts
 * under a finger, and the whole character set can be read off the keyboard
 * without pressing anything:
 *
 *              digits          QWERTY row     home row    , .
 *     shift    ! @ # $ % ^ & * ( )                        < >
 *     fn       ` ~     - _ + =    { } [ ] \ |   ; : ' "   / ?
 *
 * which is the whole of a US keyboard — every printable character it has, at or
 * near where it puts them — and the capitals, which are the caps themselves.
 *
 * BOTH MODIFIERS ARE STICKY FOR ONE KEY, spent by whatever is pressed next, and
 * go blue the moment they are up. Shift alone can latch: a DOUBLE tap locks
 * caps, where the key reads "caps" instead of "shift" and stays blue, and any
 * single tap lets go. fn never latches, and is lettered in its own blue at rest
 * so that it reads with the marks it reaches.
 *
 * SPACE TWICE, QUICKLY, ENDS THE SENTENCE — ". " and shift armed for the letter
 * that starts the next one, as a phone keyboard does. Only where a word
 * actually ended.
 *
 * What is around it — which field the keys are pointed at, lifting the screen
 * clear of them, the crossed-out keyboard putting them away — is not here: that
 * is lcd_keyboard.cpp, which owns the policy and treats this as the keys alone.
 *
 * Kept close to upstream for now (same structure, same internal names) so its
 * fixes stay easy to read across. Divergences so far:
 *   • renamed (lv_keyboard → lcd_keys, LV_KEYBOARD → LCD_KEYS) onto lcd_keygrid
 *   • lifted out of LVGL's Kconfig gates and its property table dropped
 *   • ships its own look (lcd_keys_apply_look): LVGL's theme dresses widgets by
 *     comparing class pointers, so nothing off its list gets so much as a key
 *     background — the appearance is ours now, which was half the point
 *   • the text layout above: a number row, constant caps with the modifiers'
 *     alternates printed in their corners, sticky shift and fn in place of
 *     upstream's abc/ABC/1# mode keys, and no cursor arrows
 *   • the keyboard is as tall as the map it is showing, one row being a fixed
 *     slice of the panel, instead of upstream's flat half of the screen
 *   • a key sink (lcd_keys_set_key_sink): keys as LV_EVENT_KEY, for a terminal
 *     and anything else that takes keys rather than holding text
 *
 * Still to come: the long-press chooser, for the accented forms of a letter.
 */

#ifndef LCD_KEYS_H
#define LCD_KEYS_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lcd_keygrid.h"

/*********************
 *      DEFINES
 *********************/
#define LCD_KEYS_CTRL_BUTTON_FLAGS (LCD_KEYGRID_CTRL_NO_REPEAT | LCD_KEYGRID_CTRL_CLICK_TRIG | LCD_KEYGRID_CTRL_CHECKED)

/**********************
 *      TYPEDEFS
 **********************/

typedef struct _lcd_keys_t lcd_keys_t;   /* upstream keeps this in lv_types.h */

/** Current keyboard mode.*/
typedef enum {
    LCD_KEYS_MODE_TEXT_LOWER,
    LCD_KEYS_MODE_TEXT_UPPER,
    LCD_KEYS_MODE_SPECIAL,
    LCD_KEYS_MODE_NUMBER,
    LCD_KEYS_MODE_USER_1,
    LCD_KEYS_MODE_USER_2,
    LCD_KEYS_MODE_USER_3,
    LCD_KEYS_MODE_USER_4,
#if LV_USE_ARABIC_PERSIAN_CHARS == 1
    LCD_KEYS_MODE_TEXT_ARABIC
#endif
} lcd_keys_mode_t;

LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t lcd_keys_class;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Create a Keyboard object
 * @param parent    pointer to an object, it will be the parent of the new keyboard
 * @return          pointer to the created keyboard object
 */
lv_obj_t * lcd_keys_create(lv_obj_t * parent);

/*=====================
 * Setter functions
 *====================*/

/**
 * Assign a text area to the keyboard. Pressed characters will be inserted there.
 * @param kb        pointer to a keyboard object
 * @param ta        pointer to a text area object to write into
 */
void lcd_keys_set_textarea(lv_obj_t * kb, lv_obj_t * ta);

/**
 * Assign a KEY SINK instead of a text area: every key press is delivered to
 * `sink` as LV_EVENT_KEY carrying the character (or LV_KEY_ENTER, LV_KEY_BACKSPACE,
 * LV_KEY_LEFT, LV_KEY_RIGHT), which is exactly what a keypad input device sends
 * to the object it has focused. That is what a terminal wants — there is no
 * field to type into, only a stream of keys — and it is the difference between
 * this keyboard and a real one being nothing at all.
 *
 * Enter, in this mode, also fires LV_EVENT_READY on the keyboard: the line is
 * done, and whoever put the keyboard up decides whether that means putting it
 * away. A sink and a text area are exclusive; setting one clears the other.
 *
 * @param kb        pointer to a keyboard object
 * @param sink      the object to send keys to, or NULL to go back to a text area
 */
void lcd_keys_set_key_sink(lv_obj_t * kb, lv_obj_t * sink);

/**
 * Set a new mode (e.g., text, number, special characters). Shift is settled
 * with it — LCD_KEYS_MODE_TEXT_UPPER means caps locked, anything else means
 * shift off — so a keyboard left locked by one field opens unlocked for the
 * next.
 * @param kb        pointer to a keyboard object
 * @param mode      the desired mode (see 'lcd_keys_mode_t')
 */
void lcd_keys_set_mode(lv_obj_t * kb, lcd_keys_mode_t mode);

/**
 * Enable or disable popovers showing button titles on press.
 * @param kb        pointer to a keyboard object
 * @param en        true to enable popovers; false to disable
 */
void lcd_keys_set_popovers(lv_obj_t * kb, bool en);

/**
 * Set a custom button map for the keyboard.
 * @param kb        pointer to a keyboard object
 * @param mode      the mode to assign the new map to (see 'lcd_keys_mode_t')
 * @param map       pointer to a string array describing the button map
 *                  see 'lcd_keygrid_set_map()' for more details
 * @param ctrl_map  pointer to the control map. See 'lcd_keygrid_set_ctrl_map()'
 */
void lcd_keys_set_map(lv_obj_t * kb, lcd_keys_mode_t mode, const char * const map[],
                         const lcd_keygrid_ctrl_t ctrl_map[]);

/*=====================
 * Getter functions
 *====================*/

/**
 * Get the text area currently assigned to the keyboard.
 * @param kb        pointer to a keyboard object
 * @return          pointer to the assigned text area object
 */
lv_obj_t * lcd_keys_get_textarea(const lv_obj_t * kb);

/**
 * Get the current mode of the keyboard.
 * @param kb        pointer to a keyboard object
 * @return          the current mode (see 'lcd_keys_mode_t')
 */
lcd_keys_mode_t lcd_keys_get_mode(const lv_obj_t * kb);

/**
 * Check whether popovers are enabled on the keyboard.
 * @param obj       pointer to a keyboard object
 * @return          true if popovers are enabled; false otherwise
 */
bool lcd_keys_get_popovers(const lv_obj_t * obj);

/**
 * Get the current button map of the keyboard.
 * @param kb        pointer to a keyboard object
 * @return          pointer to the map array
 */
const char * const * lcd_keys_get_map_array(const lv_obj_t * kb);

/**
 * Get the index of the last selected button (pressed, released, focused, etc.).
 * Useful in the `event_cb` to retrieve button text or properties.
 * @param obj       pointer to a keyboard object
 * @return          index of the last interacted button
 *                  returns LCD_KEYGRID_BUTTON_NONE if not set
 */
uint32_t lcd_keys_get_selected_button(const lv_obj_t * obj);

/**
 * Get the text of a button by index.
 * @param obj       pointer to a keyboard object
 * @param btn_id    index of the button (excluding newline characters)
 * @return          pointer to the text of the button
 */
const char * lcd_keys_get_button_text(const lv_obj_t * obj, uint32_t btn_id);

/*=====================
 * Other functions
 *====================*/

/**
 * Default keyboard event callback to handle button presses.
 * Adds characters to the text area and switches map if needed.
 * If a custom `event_cb` is used, this function can be called within it.
 * @param e         the triggering event
 */
void lcd_keys_def_event_cb(lv_event_t * e);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LCD_KEYS_H*/
