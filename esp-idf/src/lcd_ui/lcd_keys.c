
/**
 * lcd_keys.c — the on-screen keyboard widget, forked from LVGL v9's
 * lv_keyboard (MIT). See lcd_keys.h for what it is and how it diverges.
 */

/*********************
 *      INCLUDES
 *********************/
#include "lcd_keys_private.h"
#include "core/lv_obj_class_private.h"
#include "widgets/textarea/lv_textarea.h"
#include "misc/lv_assert.h"
#include "stdlib/lv_string.h"

#if LV_USE_TEXTAREA == 0
    #error "lv_textarea is required. Enable it in lv_conf.h (LV_USE_TEXTAREA  1) "
#endif

/*********************
 *      DEFINES
 *********************/
#define MY_CLASS (&lcd_keys_class)
#define LV_KB_BTN(width) LCD_KEYGRID_CTRL_POPOVER | width

#ifndef LCD_KEYS_CTRL_BUTTON_MODE_TEXT_LOWER
    #define LCD_KEYS_CTRL_BUTTON_MODE_TEXT_LOWER     "abc"
#endif
#ifndef LCD_KEYS_CTRL_BUTTON_MODE_TEXT_UPPER
    #define LCD_KEYS_CTRL_BUTTON_MODE_TEXT_UPPER     "ABC"
#endif
#ifndef LCD_KEYS_CTRL_BUTTON_MODE_SPECIAL
    #define LCD_KEYS_CTRL_BUTTON_MODE_SPECIAL        "1#"
#endif
#ifndef LCD_KEYS_CTRL_BUTTON_MODE_TEXT_ARABIC
    #define LCD_KEYS_CTRL_BUTTON_MODE_TEXT_ARABIC    "أب"
#endif

/* The two legends the shift key wears, spelt out the way a keyboard spells
 * them: "shift" while it is off or armed for one letter, "caps" once locked. */
#define LCD_KEYS_SHIFT_LEGEND        "shift"
#define LCD_KEYS_SHIFT_LEGEND_LOCK   "caps"

/* Two taps on one key inside this are one double tap: shift locking caps,
 * space ending a sentence. The panel's own multi-click gesture (lcd_lvgl.cpp)
 * uses the same 450 ms — a double tap should mean a double tap everywhere. */
#define LCD_KEYS_MULTICLICK_MS       450

/* A KEY ROW IS A FIXED SLICE OF THE PANEL, ‰ of its height, and the keyboard is
 * as tall as the map it is showing (lcd_keys_size_to_map): a key is the same
 * height under five rows of letters as under the number pad's four. */
#define LCD_KEYS_ROW_H_PERMILLE      98

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void lcd_keys_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj);

static void lcd_keys_update_map(lv_obj_t * obj);

static void lcd_keys_update_ctrl_map(lv_obj_t * obj);

static void lcd_keys_set_shift(lv_obj_t * obj, lcd_keys_shift_t shift);

static void lcd_keys_set_fn(lv_obj_t * obj, bool on);

static void lcd_keys_spend(lv_obj_t * obj, lcd_keys_shift_t shift_at_press, bool fn_at_press);

static bool lcd_keys_end_sentence(lv_obj_t * ta);

static const char * lcd_keys_output(lv_obj_t * obj, const char * txt, uint32_t btn_id,
                                    bool shift, bool fn, char * buf, size_t buf_len);

static void lcd_keys_dress(lv_obj_t * obj, const char * cell, lcd_keygrid_ctrl_t bits);

static void lcd_keys_size_to_map(lv_obj_t * obj);

/**********************
 *  STATIC VARIABLES
 **********************/
/* Upstream's LV_USE_OBJ_PROPERTY table is dropped with the key grid's. */
const lv_obj_class_t lcd_keys_class = {
    .constructor_cb = lcd_keys_constructor,
    .width_def = LV_PCT(100),
    /* Restated for the map in place by lcd_keys_size_to_map() on every map
     * change; the value here is the five-row text keyboard, so the very first
     * layout is already the right size. */
    .height_def = LV_PCT(56),
    .instance_size = sizeof(lcd_keys_t),
    .editable = 1,
    .base_class = &lcd_keygrid_class,
    .name = "lcd_keys",
};

/* THE ONE MUTABLE CELL IN ANY MAP: the shift key's legend, rewritten in place
 * when the lock goes on or off. Only ever one keyboard is up (lcd_keyboard.cpp
 * parks a single one), so one buffer serves them all. The default handler finds
 * the key by THIS POINTER and not by its text — which is what leaves the text
 * free to change under it. The fn key is found the same way; its legend simply
 * never changes. */
static char kb_shift_lbl[8] = LCD_KEYS_SHIFT_LEGEND;
static const char kb_fn_lbl[] = "fn";

/* THE TEXT LAYOUT: five rows, staggered like a typewriter's. The Q row fills the
 * panel and sets the key; the rows under it are that same key stepped right —
 * A three tenths of one past Q, Z half a one past A — so L ends three tenths
 * past where P begins and the punctuation that finishes the Z row is as wide as
 * every letter over it.
 *
 *     |1|2|3|4|5|6|7|8|9|0|⌫|
 *     |Q |W |E |R |T |Y |U |I |O |P |
 *      ␣|A |S |D |F |G |H |J |K |L |  ␣
 *        ␣|Z |X |C |V |B |N |M |, |. |␣
 *     |shift| ⌨̸ | fn |  space  |  ⏎  |
 *
 * The grid cannot step a row on its own — a row is filled edge to edge — so
 * each offset is a KEY: the HIDDEN spacers, laid out and drawn as nothing
 * (lcd_keygrid draw_main skips them, and a press on one selects nothing). In
 * the staggered rows a width is TENTHS of a key, which is what lets a spacer
 * be three tenths of one; the two uniform rows just share what they have.
 *
 * ONE MAP, AND IT NEVER CHANGES. The caps are printed the way a physical
 * keyboard prints them — upper case, with what shift makes in the top-right
 * corner and what fn makes in the top-left — so a modifier changes what a key
 * PRODUCES and never what it says. Nothing moves under a finger, and the
 * operator can read the whole character set off the keyboard without pressing
 * anything. */
static const char * const default_kb_map_text[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
                                                   "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
                                                   " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
                                                   " ", "Z", "X", "C", "V", "B", "N", "M", ",", ".", " ", "\n",
                                                   kb_shift_lbl, LV_SYMBOL_KEYBOARD, kb_fn_lbl, " ", LV_SYMBOL_NEW_LINE, ""
                                                  };

static const lcd_keygrid_ctrl_t default_kb_ctrl_text[] = {
    LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LCD_KEYGRID_CTRL_CHECKED | 1,
    LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1),
    LCD_KEYGRID_CTRL_HIDDEN | 2, LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LCD_KEYGRID_CTRL_HIDDEN | 6,
    LCD_KEYGRID_CTRL_HIDDEN | 7, LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LCD_KEYGRID_CTRL_HIDDEN | 1,
    LCD_KEYS_CTRL_BUTTON_FLAGS | 3, LCD_KEYS_CTRL_BUTTON_FLAGS | LCD_KEYGRID_CTRL_STRUCK | 3, LCD_KEYS_CTRL_BUTTON_FLAGS | 3, 9, LCD_KEYGRID_CTRL_CHECKED | 4
};

/* WHAT THE MODIFIERS MAKE, AND — BEING THE SAME THING — WHAT IS PRINTED IN THE
 * KEY'S CORNERS. Two arrays parallel to the map's buttons, spacers and all.
 *
 * NULL is a key that is not a cap at all: a modifier does nothing to it and its
 * legend stays centred (⌫, ⏎, the modifiers themselves, ✓). "" is a cap with
 * that corner empty — a letter, whose shifted form is the cap already printed
 * on it.
 *
 * Between them and the caps this is the whole of a US keyboard: every printable
 * character it has, at or near where a US layout puts it. */
static const char * const kb_shift_text[] = {"!", "@", "#", "$", "%", "^", "&", "*", "(", ")", NULL,
                                             "", "", "", "", "", "", "", "", "", "",
                                             NULL, "", "", "", "", "", "", "", "", "", NULL,
                                             NULL, "", "", "", "", "", "", "", "<", ">", NULL,
                                             NULL, NULL, NULL, NULL, NULL
                                            };

static const char * const kb_fn_text[] = {"`", "~", "", "", "", "", "-", "_", "+", "=", NULL,
                                          "", "", "", "", "{", "}", "[", "]", "\\", "|",
                                          NULL, "", "", "", "", "", ";", ":", "'", "\"", NULL,
                                          NULL, "", "", "", "", "", "", "", "/", "?", NULL,
                                          NULL, NULL, NULL, NULL, NULL
                                         };

#if LV_USE_ARABIC_PERSIAN_CHARS == 1
static const char * const default_kb_map_ar[] = {
    LCD_KEYS_CTRL_BUTTON_MODE_SPECIAL, "ض", "ص", "ث", "ق", "ف", "غ", "ع", "ه", "خ", "ح", "ج", "\n",
    "ش", "س", "ي", "ب", "ل", "ا", "ت", "ن", "م", "ك", "ط", LV_SYMBOL_BACKSPACE, "\n",
    "ذ", "ء", "ؤ", "ر", "ى", "ة", "و", "ز", "ظ", "د", "ز", "ظ", "د", "\n",
    LV_SYMBOL_CLOSE, LCD_KEYS_CTRL_BUTTON_MODE_TEXT_LOWER, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_NEW_LINE, LV_SYMBOL_OK, ""
};

static const lcd_keygrid_ctrl_t default_kb_ctrl_ar_map[] = {
    LCD_KEYS_CTRL_BUTTON_FLAGS | 5, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 7,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    LCD_KEYS_CTRL_BUTTON_FLAGS | 2, LCD_KEYS_CTRL_BUTTON_FLAGS | 2, 2, 6, 2, 3, LCD_KEYS_CTRL_BUTTON_FLAGS | 2
};
#endif

static const char * const default_kb_map_spec[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
                                                   LCD_KEYS_CTRL_BUTTON_MODE_TEXT_LOWER, "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
                                                   "\\",  "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
                                                   LV_SYMBOL_CLOSE,
#if LV_USE_ARABIC_PERSIAN_CHARS == 1
                                                   LCD_KEYS_CTRL_BUTTON_MODE_TEXT_ARABIC,
#endif
                                                   LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
                                                  };

static const lcd_keygrid_ctrl_t default_kb_ctrl_spec_map[] = {
    LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LCD_KEYGRID_CTRL_CHECKED | 2,
    LCD_KEYS_CTRL_BUTTON_FLAGS | 2, LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1),
    LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1),
    LCD_KEYS_CTRL_BUTTON_FLAGS | 2,
#if LV_USE_ARABIC_PERSIAN_CHARS == 1
    LCD_KEYS_CTRL_BUTTON_FLAGS | 2,
#endif
    LCD_KEYGRID_CTRL_CHECKED | 2, 6, LCD_KEYGRID_CTRL_CHECKED | 2, LCD_KEYS_CTRL_BUTTON_FLAGS | 2
};

static const char * const default_kb_map_num[] = {"1", "2", "3", LV_SYMBOL_CLOSE, "\n",
                                                  "4", "5", "6", LV_SYMBOL_OK, "\n",
                                                  "7", "8", "9", LV_SYMBOL_BACKSPACE, "\n",
                                                  "+/-", "0", ".", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, ""
                                                 };

static const lcd_keygrid_ctrl_t default_kb_ctrl_num_map[] = {
    1, 1, 1, LCD_KEYS_CTRL_BUTTON_FLAGS | 2,
    1, 1, 1, LCD_KEYS_CTRL_BUTTON_FLAGS | 2,
    1, 1, 1, 2,
    1, 1, 1, 1, 1
};

/* TEXT_LOWER and TEXT_UPPER are the same keys — the caps never change, shift
 * does — so both name the one text map, and the mode only says which way a
 * keyboard opens. */
static const char * const * kb_map[10] = {
    default_kb_map_text,
    default_kb_map_text,
    default_kb_map_spec,
    default_kb_map_num,
    default_kb_map_text,
    default_kb_map_text,
    default_kb_map_text,
    default_kb_map_text,
#if LV_USE_ARABIC_PERSIAN_CHARS == 1
    default_kb_map_ar,
#endif
    NULL
};
static const lcd_keygrid_ctrl_t * kb_ctrl[10] = {
    default_kb_ctrl_text,
    default_kb_ctrl_text,
    default_kb_ctrl_spec_map,
    default_kb_ctrl_num_map,
    default_kb_ctrl_text,
    default_kb_ctrl_text,
    default_kb_ctrl_text,
    default_kb_ctrl_text,
#if LV_USE_ARABIC_PERSIAN_CHARS == 1
    default_kb_ctrl_ar_map,
#endif
    NULL
};

/* Only the text layout carries corner marks; the number pad and the symbol map
 * are plain keys that type what they say. */
static const char * const * kb_mark_tl[10] = {kb_fn_text, kb_fn_text, NULL, NULL,
                                              kb_fn_text, kb_fn_text, kb_fn_text, kb_fn_text, NULL, NULL
                                             };
static const char * const * kb_mark_tr[10] = {kb_shift_text, kb_shift_text, NULL, NULL,
                                              kb_shift_text, kb_shift_text, kb_shift_text, kb_shift_text, NULL, NULL
                                             };

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * lcd_keys_create(lv_obj_t * parent)
{
    LV_LOG_INFO("begin");
    lv_obj_t * obj = lv_obj_class_create_obj(&lcd_keys_class, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

/*=====================
 * Setter functions
 *====================*/

void lcd_keys_set_textarea(lv_obj_t * obj, lv_obj_t * ta)
{
    if(ta) {
        LV_ASSERT_OBJ(ta, &lv_textarea_class);
    }

    LV_ASSERT_OBJ(obj, MY_CLASS);
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;

    /*Hide the cursor of the old Text area if cursor management is enabled*/
    if(keyboard->ta) {
        lv_obj_remove_state(keyboard->ta, LV_STATE_FOCUSED);
    }

    keyboard->ta = ta;
    if(ta) keyboard->sink = NULL;

    /*Show the cursor of the new Text area if cursor management is enabled*/
    if(keyboard->ta) {
        lv_obj_add_state(keyboard->ta, LV_STATE_FOCUSED);
    }
}

void lcd_keys_set_key_sink(lv_obj_t * obj, lv_obj_t * sink)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;

    keyboard->sink = sink;
    if(sink) lcd_keys_set_textarea(obj, NULL);
}

void lcd_keys_set_mode(lv_obj_t * obj, lcd_keys_mode_t mode)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;

    /* Being told the mode outright settles the modifiers with it — TEXT_UPPER
     * IS caps locked — so a keyboard left locked, or with fn armed, by the last
     * field opens clean for the next. Asked for the mode already up, with the
     * modifiers where they belong, there is nothing to do. */
    lcd_keys_shift_t shift = mode == LCD_KEYS_MODE_TEXT_UPPER ? LCD_KEYS_SHIFT_LOCK
                                                              : LCD_KEYS_SHIFT_OFF;
    if(keyboard->mode == mode && keyboard->shift == shift && !keyboard->fn) return;

    keyboard->mode = mode;
    keyboard->shift = shift;
    keyboard->fn = 0;
    lcd_keys_update_map(obj);
}

void lcd_keys_set_popovers(lv_obj_t * obj, bool en)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;

    if(keyboard->popovers == en) {
        return;
    }

    keyboard->popovers = en;
    /*Through the map, not the ctrl map alone: rewriting the ctrl bits puts the
     *modifier keys back to their resting look, and update_map undoes that*/
    lcd_keys_update_map(obj);
}

void lcd_keys_set_map(lv_obj_t * obj, lcd_keys_mode_t mode, const char * const map[],
                         const lcd_keygrid_ctrl_t ctrl_map[])
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    kb_map[mode] = map;
    kb_ctrl[mode] = ctrl_map;
    /* The built-in marks belong to the built-in map, and are indexed by ITS
     * buttons: a caller's own map arrives without them. */
    kb_mark_tl[mode] = NULL;
    kb_mark_tr[mode] = NULL;
    lcd_keys_update_map(obj);
}

/*=====================
 * Getter functions
 *====================*/

lv_obj_t * lcd_keys_get_textarea(const lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    return keyboard->ta;
}

lcd_keys_mode_t lcd_keys_get_mode(const lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    return keyboard->mode;
}

bool lcd_keys_get_popovers(const lv_obj_t * obj)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    return keyboard->popovers;
}

/*=====================
 * Other functions
 *====================*/

void lcd_keys_def_event_cb(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_current_target(e);

    LV_ASSERT_OBJ(obj, MY_CLASS);
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    uint32_t btn_id = lcd_keygrid_get_selected_button(obj);
    if(btn_id == LCD_KEYGRID_BUTTON_NONE) return;

    const char * txt = lcd_keygrid_get_button_text(obj, btn_id);
    if(txt == NULL) return;

    /* Two taps on the same key, close together — the gesture that locks caps
     * and the one that ends a sentence. Recorded before anything returns, so a
     * modifier press counts as a tap like any other. */
    const bool dbl = btn_id == keyboard->last_btn &&
                     lv_tick_elaps(keyboard->last_tick) <= LCD_KEYS_MULTICLICK_MS;
    uint32_t now = lv_tick_get();
    keyboard->last_btn  = btn_id;
    keyboard->last_tick = now ? now : 1;

    /* Where the modifiers stood when this key went down. A key may arm shift on
     * its way out (space, ending a sentence), and what it armed must not then
     * be spent by the very press that armed it. */
    const lcd_keys_shift_t shift_at_press = (lcd_keys_shift_t)keyboard->shift;
    const bool fn_at_press = keyboard->fn;

    /* The modifiers, recognised by the map cell they are rather than by the
     * legend they wear. Shift arms for one key, and only a DOUBLE tap locks it
     * — locking is a gesture of its own, not a stop on the way round. fn arms
     * for one key and has no lock at all. */
    if(txt == kb_shift_lbl) {
        lcd_keys_set_shift(obj, dbl ? LCD_KEYS_SHIFT_LOCK :
                                shift_at_press == LCD_KEYS_SHIFT_OFF ? LCD_KEYS_SHIFT_ONCE
                                                                     : LCD_KEYS_SHIFT_OFF);
        return;
    }
    if(txt == kb_fn_lbl) {
        lcd_keys_set_fn(obj, !fn_at_press);
        return;
    }

    /* The mode keys. None of them is on the layouts here any more — shift
     * carries the case and the symbols — but a map installed by lcd_keys_set_map
     * may still name them, and switching a map is what they mean. */
    if(lv_strcmp(txt, LCD_KEYS_CTRL_BUTTON_MODE_TEXT_LOWER) == 0) {
        lcd_keys_set_mode(obj, LCD_KEYS_MODE_TEXT_LOWER);
        return;
    }
#if LV_USE_ARABIC_PERSIAN_CHARS == 1
    else if(lv_strcmp(txt, LCD_KEYS_CTRL_BUTTON_MODE_TEXT_ARABIC) == 0) {
        lcd_keys_set_mode(obj, LCD_KEYS_MODE_TEXT_ARABIC);
        return;
    }
#endif
    else if(lv_strcmp(txt, LCD_KEYS_CTRL_BUTTON_MODE_TEXT_UPPER) == 0) {
        lcd_keys_set_mode(obj, LCD_KEYS_MODE_TEXT_UPPER);
        return;
    }
    else if(lv_strcmp(txt, LCD_KEYS_CTRL_BUTTON_MODE_SPECIAL) == 0) {
        lcd_keys_set_mode(obj, LCD_KEYS_MODE_SPECIAL);
        return;
    }
    else if(lv_strcmp(txt, LV_SYMBOL_CLOSE) == 0 || lv_strcmp(txt, LV_SYMBOL_KEYBOARD) == 0) {
        lv_result_t res = lv_obj_send_event(obj, LV_EVENT_CANCEL, NULL);
        if(res != LV_RESULT_OK) return;

        if(keyboard->ta) {
            res = lv_obj_send_event(keyboard->ta, LV_EVENT_CANCEL, NULL);
            if(res != LV_RESULT_OK) return;
        }
        return;
    }
    else if(lv_strcmp(txt, LV_SYMBOL_OK) == 0) {
        lv_result_t res = lv_obj_send_event(obj, LV_EVENT_READY, NULL);
        if(res != LV_RESULT_OK) return;

        if(keyboard->ta) {
            res = lv_obj_send_event(keyboard->ta, LV_EVENT_READY, NULL);
            if(res != LV_RESULT_OK) return;
        }
        return;
    }

    /* A SINK TAKES KEYS, NOT TEXT — what a keypad input device would send to the
     * object it has focused. There is no field to type into behind a terminal,
     * only a stream of keys, and this is what makes the two the same thing. */
    if(keyboard->sink) {
        uint32_t key;
        char out[8];
        if(lv_strcmp(txt, "Enter") == 0 || lv_strcmp(txt, LV_SYMBOL_NEW_LINE) == 0) key = LV_KEY_ENTER;
        else if(lv_strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) key = LV_KEY_BACKSPACE;
        else if(lv_strcmp(txt, LV_SYMBOL_LEFT) == 0)      key = LV_KEY_LEFT;
        else if(lv_strcmp(txt, LV_SYMBOL_RIGHT) == 0)     key = LV_KEY_RIGHT;
        else {
            /*Every character these layouts can produce is one ASCII byte*/
            key = (uint8_t)lcd_keys_output(obj, txt, btn_id, shift_at_press != LCD_KEYS_SHIFT_OFF,
                                           fn_at_press, out, sizeof(out))[0];
        }
        if(lv_obj_is_valid(keyboard->sink)) lv_obj_send_event(keyboard->sink, LV_EVENT_KEY, &key);
        lcd_keys_spend(obj, shift_at_press, fn_at_press);
        /*The line is done. What that means for the keyboard is not ours to say*/
        if(key == LV_KEY_ENTER) lv_obj_send_event(obj, LV_EVENT_READY, NULL);
        return;
    }

    /*Add the characters to the text area if set*/
    if(keyboard->ta == NULL || !lv_obj_is_valid(keyboard->ta)) {
        lcd_keys_spend(obj, shift_at_press, fn_at_press);
        return;
    }

    if(lv_strcmp(txt, "Enter") == 0 || lv_strcmp(txt, LV_SYMBOL_NEW_LINE) == 0) {
        if(lv_textarea_get_one_line(keyboard->ta)) {
            /* Enter finishes a one-line field, as it does on a keyboard with
             * keys. The keyboard goes first (READY on us) and the field is told
             * after: answering may well rebuild the screen it was standing
             * over, and it must not do that with the keys still up. */
            lv_obj_t * ta = keyboard->ta;
            lv_result_t res = lv_obj_send_event(obj, LV_EVENT_READY, NULL);
            if(res != LV_RESULT_OK) return;
            if(lv_obj_is_valid(ta)) lv_obj_send_event(ta, LV_EVENT_READY, NULL);
            return;
        }
        lv_textarea_add_char(keyboard->ta, '\n');
    }
    else if(lv_strcmp(txt, LV_SYMBOL_LEFT) == 0) {
        lv_textarea_cursor_left(keyboard->ta);
    }
    else if(lv_strcmp(txt, LV_SYMBOL_RIGHT) == 0) {
        lv_textarea_cursor_right(keyboard->ta);
    }
    else if(lv_strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(keyboard->ta);
    }
    else if(lv_strcmp(txt, "+/-") == 0) {
        uint32_t cur        = lv_textarea_get_cursor_pos(keyboard->ta);
        const char * ta_txt = lv_textarea_get_text(keyboard->ta);
        if(ta_txt[0] == '-') {
            lv_textarea_set_cursor_pos(keyboard->ta, 1);
            lv_textarea_delete_char(keyboard->ta);
            lv_textarea_add_char(keyboard->ta, '+');
            lv_textarea_set_cursor_pos(keyboard->ta, cur);
        }
        else if(ta_txt[0] == '+') {
            lv_textarea_set_cursor_pos(keyboard->ta, 1);
            lv_textarea_delete_char(keyboard->ta);
            lv_textarea_add_char(keyboard->ta, '-');
            lv_textarea_set_cursor_pos(keyboard->ta, cur);
        }
        else {
            lv_textarea_set_cursor_pos(keyboard->ta, 0);
            lv_textarea_add_char(keyboard->ta, '-');
            lv_textarea_set_cursor_pos(keyboard->ta, cur + 1);
        }
    }
    /* SPACE TWICE, QUICKLY, ENDS THE SENTENCE: the space just typed becomes
     * ". " and shift arms itself for the letter that starts the next one. Only
     * where a word actually ended — anywhere else two spaces are two spaces. */
    else if(dbl && txt[0] == ' ' && txt[1] == '\0' && lcd_keys_end_sentence(keyboard->ta)) {
        lcd_keys_set_shift(obj, LCD_KEYS_SHIFT_ONCE);
    }
    else {
        char out[8];
        lv_textarea_add_text(keyboard->ta,
                             lcd_keys_output(obj, txt, btn_id,
                                             shift_at_press != LCD_KEYS_SHIFT_OFF, fn_at_press,
                                             out, sizeof(out)));
    }

    lcd_keys_spend(obj, shift_at_press, fn_at_press);
}

const char * const * lcd_keys_get_map_array(const lv_obj_t * kb)
{
    return lcd_keygrid_get_map(kb);
}

uint32_t lcd_keys_get_selected_button(const lv_obj_t * obj)
{
    return lcd_keygrid_get_selected_button(obj);
}

const char * lcd_keys_get_button_text(const lv_obj_t * obj, uint32_t btn_id)
{
    return lcd_keygrid_get_button_text(obj, btn_id);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/* THE KEYS DRESS THEMSELVES.
 *
 * LVGL's default theme decides what a widget looks like by comparing CLASS
 * POINTERS (`lv_obj_check_type(obj, &lv_keyboard_class)` and friends), so the
 * moment this became a class of our own the theme stopped recognising it and
 * applied nothing at all: keys with no background and no text colour, laid out
 * and hit-tested perfectly and drawn as nothing. Every fork of an LVGL widget
 * meets this, and the answer is not to chase the theme — it is that a widget
 * off the theme's list ships its own look.
 *
 * Which is what we wanted anyway: this IS the keyboard's appearance now, in one
 * place, ours to change. Dark keys with light legends, the pressed key picked
 * out, the mode/utility keys (the map marks them CHECKED) sunk a shade so they
 * read as chrome rather than letters. */
static void lcd_keys_apply_look(lv_obj_t * obj)
{
    /* The pad itself. It is the whole keyboard now — there is no layer behind it
     * any more, only the app it has risen over — so it carries its own ground,
     * and a small even gap so the keys are the rest of the picture. */
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 3, 0);
    lv_obj_set_style_pad_row(obj, 3, 0);
    lv_obj_set_style_pad_column(obj, 3, 0);

    /* A key. */
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x2a313a), LV_PART_ITEMS);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xe6ebf0), LV_PART_ITEMS);
    lv_obj_set_style_radius(obj, 4, LV_PART_ITEMS);
    lv_obj_set_style_border_width(obj, 0, LV_PART_ITEMS);

    /* Pressed: bright enough to be seen under the finger that is covering it. */
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x4a90d9), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(obj, lv_color_white(), LV_PART_ITEMS | LV_STATE_PRESSED);

    /* CHECKED is how the maps mark the backspace, ⏎, ✓ and the modifiers at
     * rest — chrome, not letters, so they sit back. */
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x1b2027), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xa8b3c0), LV_PART_ITEMS | LV_STATE_CHECKED);

    /* USER_1 is a modifier that is UP — armed for the next key or, for shift,
     * locked. It wears the pressed colour because that is what it is: a key
     * being held down, by the operator or by itself. */
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x4a90d9), LV_PART_ITEMS | LV_STATE_USER_1);
    lv_obj_set_style_text_color(obj, lv_color_white(), LV_PART_ITEMS | LV_STATE_USER_1);

    /* Both bits: a chrome key with a tinted legend, which is fn at rest — it is
     * labelled in the blue of the marks it reaches. */
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x1b2027), LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_USER_1);
    lv_obj_set_style_text_color(obj, lv_color_hex(0x3a86ff), LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_USER_1);

    lv_obj_set_style_bg_color(obj, lv_color_hex(0x20252c), LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_text_color(obj, lv_color_hex(0x555f6b), LV_PART_ITEMS | LV_STATE_DISABLED);

    /* The corner marks. Shift's, top right, is the legend's own colour — it is
     * one of the letters, only smaller and up in the corner. fn's, top left, is
     * blue, which is the whole way an operator knows which modifier reaches it.
     * (Their sizes are set per open, with the legend's, in lcd_keyboard.cpp: a
     * font can be freed under us.) */
    lv_obj_set_style_text_color(obj, lv_color_hex(0xe6ebf0), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(obj, lv_color_hex(0x3a86ff), LV_PART_CUSTOM_FIRST);

    /* The line ruled through the ✗ key's keyboard glyph — "put this away". */
    lv_obj_set_style_line_color(obj, lv_color_hex(0xe05252), LV_PART_ITEMS);
    lv_obj_set_style_line_width(obj, 2, LV_PART_ITEMS);
}

static void lcd_keys_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj)
{
    LV_UNUSED(class_p);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    keyboard->ta         = NULL;
    keyboard->sink       = NULL;
    keyboard->mode       = LCD_KEYS_MODE_TEXT_LOWER;
    keyboard->shift      = LCD_KEYS_SHIFT_OFF;
    keyboard->fn         = 0;
    keyboard->popovers   = 0;
    keyboard->last_btn   = LCD_KEYGRID_BUTTON_NONE;
    keyboard->last_tick  = 0;

    lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(obj, lcd_keys_def_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_base_dir(obj, LV_BASE_DIR_LTR, 0);
    lcd_keys_apply_look(obj);

    lcd_keys_update_map(obj);
}

/**
 * Update the key and control map for the current mode
 * @param obj pointer to a keyboard object
 */
static void lcd_keys_update_map(lv_obj_t * obj)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    lcd_keygrid_set_map(obj, kb_map[keyboard->mode]);
    lcd_keys_update_ctrl_map(obj);
    /*After the map, which drops whatever marks the last one had*/
    lcd_keygrid_set_marks(obj, kb_mark_tl[keyboard->mode], kb_mark_tr[keyboard->mode]);
    lcd_keys_size_to_map(obj);
    /*And after the ctrl map, which has just put both modifier keys back to
     *their resting look whatever the modifiers are actually doing*/
    lcd_keys_set_shift(obj, (lcd_keys_shift_t)keyboard->shift);
    lcd_keys_set_fn(obj, keyboard->fn);
}

/**
 * Take the height the map in place asks for: one row is
 * LCD_KEYS_ROW_H_PERMILLE of the panel, whatever the map, plus the gaps
 * between the rows and the pad around them.
 * @param obj pointer to a keyboard object
 */
static void lcd_keys_size_to_map(lv_obj_t * obj)
{
    lcd_keygrid_t * btnm = (lcd_keygrid_t *)obj;
    if(btnm->row_cnt == 0) return;

    int32_t vres = lv_display_get_vertical_resolution(lv_obj_get_display(obj));
    int32_t gap  = lv_obj_get_style_pad_row(obj, LV_PART_MAIN);
    int32_t pad  = lv_obj_get_style_pad_top(obj, LV_PART_MAIN) +
                   lv_obj_get_style_pad_bottom(obj, LV_PART_MAIN);
    /* Rows scaled TOGETHER rather than one at a time: at ~23 px a row, dropping
     * the remainder per row costs the keyboard most of a row over five. */
    lv_obj_set_height(obj, (vres * (int32_t)btnm->row_cnt * LCD_KEYS_ROW_H_PERMILLE) / 1000
                           + ((int32_t)btnm->row_cnt - 1) * gap + pad);
}

/**
 * Find the button carrying a given cell of the map, BY POINTER — the modifier
 * keys are known by the cell they are, not by the legend they wear.
 * @param obj  pointer to a keyboard object
 * @param cell the map entry to look for
 * @return     its button index, or LCD_KEYGRID_BUTTON_NONE if this map has none
 */
static uint32_t lcd_keys_btn_of(lv_obj_t * obj, const char * cell)
{
    lcd_keygrid_t * btnm = (lcd_keygrid_t *)obj;
    uint32_t btn = 0;
    uint32_t i;
    for(i = 0; btnm->map_p[i] && btnm->map_p[i][0] != '\0'; i++) {
        if(lv_strcmp(btnm->map_p[i], "\n") == 0) continue;
        if(btnm->map_p[i] == cell) return btn;
        btn++;
    }
    return LCD_KEYGRID_BUTTON_NONE;
}

/**
 * Put a key into exactly one of the two looks the ctrl bits can name — CHECKED
 * for chrome, ACCENT for the blue of a modifier that is up, and the two of them
 * together for the chrome key with a tinted legend that fn wears at rest.
 * @param obj  pointer to a keyboard object
 * @param cell the key's map entry
 * @param bits CHECKED, ACCENT, or both
 */
static void lcd_keys_dress(lv_obj_t * obj, const char * cell, lcd_keygrid_ctrl_t bits)
{
    uint32_t id = lcd_keys_btn_of(obj, cell);
    if(id == LCD_KEYGRID_BUTTON_NONE) return;

    lcd_keygrid_clear_button_ctrl(obj, id, LCD_KEYGRID_CTRL_CHECKED | LCD_KEYGRID_CTRL_ACCENT);
    lcd_keygrid_set_button_ctrl(obj, id, bits);
}

/**
 * Put shift where it is being asked to go. The keys do not move — the caps are
 * printed upper whatever shift is doing — so this rewrites the legend on the
 * one key and redraws it, and nothing else.
 * @param obj   pointer to a keyboard object
 * @param shift the state shift is moving to
 */
static void lcd_keys_set_shift(lv_obj_t * obj, lcd_keys_shift_t shift)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    keyboard->shift = shift;
    lv_strcpy(kb_shift_lbl, shift == LCD_KEYS_SHIFT_LOCK ? LCD_KEYS_SHIFT_LEGEND_LOCK
                                                         : LCD_KEYS_SHIFT_LEGEND);
    /* Blue the moment it is up, armed or locked alike — the legend is what says
     * which, and locked is the one that stays. */
    lcd_keys_dress(obj, kb_shift_lbl, shift == LCD_KEYS_SHIFT_OFF ? LCD_KEYGRID_CTRL_CHECKED
                                                                  : LCD_KEYGRID_CTRL_ACCENT);
}

/**
 * Arm or drop fn. It never latches, so there are only the two looks.
 * @param obj pointer to a keyboard object
 * @param on  fn is up for the next key
 */
static void lcd_keys_set_fn(lv_obj_t * obj, bool on)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    keyboard->fn = on;
    /* fn wears the blue of the marks it reaches: at rest, chrome with a blue
     * legend (CHECKED and ACCENT together); armed, the blue key with a white
     * one, like shift. */
    lcd_keys_dress(obj, kb_fn_lbl, on ? LCD_KEYGRID_CTRL_ACCENT
                                      : LCD_KEYGRID_CTRL_CHECKED | LCD_KEYGRID_CTRL_ACCENT);
}

/**
 * Spend whatever was armed when the key went down. A lock is not spent, and
 * neither is anything a key has armed on its way out — hence the state at the
 * press rather than the state now.
 * @param obj           pointer to a keyboard object
 * @param shift_at_press where shift stood when the key went down
 * @param fn_at_press    whether fn was up when it did
 */
static void lcd_keys_spend(lv_obj_t * obj, lcd_keys_shift_t shift_at_press, bool fn_at_press)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    if(shift_at_press == LCD_KEYS_SHIFT_ONCE && keyboard->shift == LCD_KEYS_SHIFT_ONCE) {
        lcd_keys_set_shift(obj, LCD_KEYS_SHIFT_OFF);
    }
    if(fn_at_press && keyboard->fn) lcd_keys_set_fn(obj, false);
}

/**
 * How many characters (not bytes) a UTF-8 string holds — a continuation byte
 * is the tail of a character already counted.
 */
static uint32_t lcd_keys_char_count(const char * s)
{
    uint32_t n = 0;
    for(; *s; s++) {
        if((*s & 0xC0) != 0x80) n++;
    }
    return n;
}

/**
 * Turn the space at the end of the text into ". ".
 * @param ta the text area being typed into
 * @return   true if it did — false means the space stands as a space, because
 *           no word ended there or the operator is typing somewhere else in the
 *           text entirely
 */
static bool lcd_keys_end_sentence(lv_obj_t * ta)
{
    const char * s = lv_textarea_get_text(ta);
    size_t len = s ? lv_strlen(s) : 0;
    if(len < 2 || s[len - 1] != ' ') return false;

    char prev = s[len - 2];
    if(prev == ' ' || prev == '\n' || prev == '.' || prev == ',' ||
       prev == '!' || prev == '?') return false;

    /*The tail above is only the operator's if the cursor is sitting on it*/
    if(lv_textarea_get_cursor_pos(ta) != lcd_keys_char_count(s)) return false;

    lv_textarea_delete_char(ta);
    lv_textarea_add_text(ta, ". ");
    return true;
}

/**
 * What a key produces: fn's mark if it carries one, else shift's, else the cap
 * itself — lower-cased when shift is off, the caps being printed upper.
 * @param obj     pointer to a keyboard object
 * @param txt     the cap
 * @param btn_id  which button it is, to index the mark arrays
 * @param shift   shift was up when the key went down
 * @param fn      fn was up when the key went down
 * @param buf     scratch for the lower-cased cap
 * @param buf_len its size
 * @return        the text to type; `txt` or `buf` or a mark, all outliving the call
 */
static const char * lcd_keys_output(lv_obj_t * obj, const char * txt, uint32_t btn_id,
                                    bool shift, bool fn, char * buf, size_t buf_len)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    const char * const * marks;

    marks = kb_mark_tl[keyboard->mode];
    if(fn && marks && marks[btn_id] && marks[btn_id][0]) return marks[btn_id];

    marks = kb_mark_tr[keyboard->mode];
    if(shift && marks && marks[btn_id] && marks[btn_id][0]) return marks[btn_id];

    if(shift) return txt;

    size_t n = lv_strlen(txt);
    if(n >= buf_len) return txt;
    size_t i;
    for(i = 0; i < n; i++) buf[i] = txt[i] >= 'A' && txt[i] <= 'Z' ? (char)(txt[i] + 32) : txt[i];
    buf[n] = '\0';
    return buf;
}

/**
 * Update the control map for the current mode
 * @param obj pointer to a keyboard object
 */
static void lcd_keys_update_ctrl_map(lv_obj_t * obj)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;

    if(keyboard->popovers) {
        /*Apply the current control map (already includes LCD_KEYGRID_CTRL_POPOVER flags)*/
        lcd_keygrid_set_ctrl_map(obj, kb_ctrl[keyboard->mode]);
    }
    else {
        /*Make a copy of the current control map*/
        lcd_keygrid_t * btnm = (lcd_keygrid_t *)obj;
        lcd_keygrid_ctrl_t * ctrl_map = lv_malloc(btnm->btn_cnt * sizeof(lcd_keygrid_ctrl_t));
        lv_memcpy(ctrl_map, kb_ctrl[keyboard->mode], sizeof(lcd_keygrid_ctrl_t) * btnm->btn_cnt);

        /*Remove all LCD_KEYGRID_CTRL_POPOVER flags*/
        uint32_t i;
        for(i = 0; i < btnm->btn_cnt; i++) {
            ctrl_map[i] &= (~LCD_KEYGRID_CTRL_POPOVER);
        }

        /*Apply new control map and clean up*/
        lcd_keygrid_set_ctrl_map(obj, ctrl_map);
        lv_free(ctrl_map);
    }
}
