
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
#include "indev/lv_indev.h"
#include "lcd_input.h"   /* LCD_KEY_CTRL */
#include "misc/lv_assert.h"
#include "misc/lv_text_private.h"   /* lv_text_encoded_next — the chooser is UTF-8 */
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

/* HOW FAR A FINGER MUST BE THROWN before the press becomes a flick, as a
 * fraction (numerator/denominator) of the pressed key's own HEIGHT — the one
 * dimension every key on a map shares, where width runs from a letter to the
 * space bar.
 *
 * It has to clear a whole key and then some. An ordinary tap on a panel this
 * size slides: measured off real use, up to about a key pitch. Under that and
 * tapping starts producing corner marks, which is far worse than a flick being
 * a little hard to make — a missed flick costs a second try, a false one
 * corrupts what was typed. */
#define LCD_KEYS_FLICK_NUM           7
#define LCD_KEYS_FLICK_DEN           5

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

static void lcd_keys_set_ctrl(lv_obj_t * obj, bool on);

static void lcd_keys_flick_cb(lv_event_t * e);

static void lcd_keys_emit(lv_obj_t * obj, const char * utf8);

static void lcd_keys_alt_open(lv_obj_t * obj, uint32_t btn_id);

static void lcd_keys_alt_track(lv_obj_t * obj, int32_t x, int32_t y);

static void lcd_keys_alt_close(lv_obj_t * obj, bool commit);

static void lcd_keys_spend(lv_obj_t * obj, lcd_keys_shift_t shift_at_press,
                           bool fn_at_press, bool ctrl_at_press);

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
static const char kb_ctrl_lbl[] = "ctrl";

/* THREE CELLS THAT WEAR THE SAME LEGEND AND MEAN DIFFERENT THINGS. The key that
 * puts the keyboard away and the key that comes back from the numbers layout are
 * both a keyboard glyph — one crossed out, one not — and a crossed-out glyph is
 * a ctrl bit, not different text. Separate arrays (never `const char *`, which a
 * compiler may fold together) give them separate addresses, and the handler
 * knows them apart by address like it already knows the modifiers. */
static const char kb_hide_lbl[] = LV_SYMBOL_KEYBOARD;   /* struck: put them away */
static const char kb_abc_lbl[]  = LV_SYMBOL_KEYBOARD;   /* back to the letters */

/* THE KEY TO THE NUMBERS LAYOUT WEARS A PICTURE OF IT: no legend of its own,
 * and the LCD_KEYGRID_CTRL_PAD bit draws the three-by-four grid of dots that is
 * that layout's pad seen from far enough away. It matches the plain keyboard
 * glyph that comes back from there: the key beside enter carries a picture of
 * where it leads in both layouts, and neither asks to be read.
 *
 * The cell is a SPACE and not an empty string: an empty cell ends a map. Its
 * address is what the handler knows it by, like the two keyboard glyphs above,
 * so the space it prints never has to be told apart from the space bar's. */
static const char kb_num_lbl[]  = " ";

/* THE TEXT LAYOUT: five rows, staggered like a typewriter's. The Q row fills the
 * panel and sets the key; the rows under it are that same key stepped right —
 * A half a key past Q, Z four tenths of one past A — so L ends half a key past
 * where P begins and the punctuation that finishes the Z row is as wide as
 * every letter over it.
 *
 *     |1|2|3|4|5|6|7|8|9|0|⌫|
 *     |Q |W |E |R |T |Y |U |I |O |P |
 *       ␣|A |S |D |F |G |H |J |K |L | ␣
 *     |fn|Z |X |C |V |B |N |M |, |. |␣
 *     |⌨̸|shift|ctl|   space   |∷| ⏎ |
 *
 * The grid cannot step a row on its own — a row is filled edge to edge — so
 * each offset is a KEY: the HIDDEN spacers, laid out and drawn as nothing
 * (lcd_keygrid draw_main skips them, and a press on one selects nothing). In
 * the staggered rows a width is TENTHS of a key, which is what lets a spacer
 * be half of one; the two uniform rows just share what they have.
 *
 * FN IS THE Z ROW'S OFFSET. The stagger has to spend a key's width on nothing
 * at the left of that row, and fn is the modifier that can live in it: it is
 * reached deliberately rather than in the flow of typing, its blue lettering
 * needs no room for a word, and standing beside the marks it reaches — the
 * top-left corner of every cap in the two rows above it — is where it explains
 * itself. What that buys is the bottom row: four keys wide instead of six, so
 * the space bar is a thumb's width of panel rather than a third of a row.
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
                                                   kb_fn_lbl, "Z", "X", "C", "V", "B", "N", "M", ",", ".", " ", "\n",
                                                   kb_hide_lbl, kb_shift_lbl, kb_ctrl_lbl, " ", kb_num_lbl, LV_SYMBOL_NEW_LINE, ""
                                                  };

/* THE BOTTOM ROW READS LEFT TO RIGHT AS "PUT THESE AWAY, THEN THE MODIFIERS,
 * THEN THE TWO WAYS OUT OF WHAT YOU ARE TYPING". Away sits in the corner, where
 * a thumb finds it without looking and where it is furthest from the modifiers
 * it must never be confused with. Shift and ctrl stand together because they
 * behave alike — armed by a tap, blue while up, spent by the next key — and
 * shift is the wider because it is pressed the more.
 *
 * ENTER HOLDS THE FAR CORNER, with the way to the other layout just inside it.
 * The corners are the two places a thumb reaches without aiming, and enter is
 * pressed many times for every once the layout is changed, so the corner is
 * enter's by rights. The other corner is away in both layouts, and the key
 * beside enter changes the layout in both — the pad in this one, the plain
 * keyboard glyph in that one — so the pair is in the same place either way.
 *
 * Thirty-three units to the row, which is what it takes to size the space bar
 * against the cap of fifteen a single key can claim. */
static const lcd_keygrid_ctrl_t default_kb_ctrl_text[] = {
    LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LCD_KEYGRID_CTRL_CHECKED | 1,
    LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1),
    LCD_KEYGRID_CTRL_HIDDEN | 5, LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LCD_KEYGRID_CTRL_HIDDEN | 5,
    LCD_KEYS_CTRL_BUTTON_FLAGS | 9, LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LV_KB_BTN(10), LCD_KEYGRID_CTRL_HIDDEN | 1,
    LCD_KEYS_CTRL_BUTTON_FLAGS | LCD_KEYGRID_CTRL_STRUCK | 3, LCD_KEYS_CTRL_BUTTON_FLAGS | 4, LCD_KEYS_CTRL_BUTTON_FLAGS | 3, 15, LCD_KEYS_CTRL_BUTTON_FLAGS | LCD_KEYGRID_CTRL_PAD | 3, LCD_KEYGRID_CTRL_CHECKED | 5
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
                                             NULL, NULL, NULL, NULL, NULL, NULL
                                            };

static const char * const kb_fn_text[] = {"`", "~", "", "", "", "", "-", "_", "+", "=", NULL,
                                          "", "", "", "", "{", "}", "[", "]", "\\", "|",
                                          NULL, "", "", "", "", "", ";", ":", "'", "\"", NULL,
                                          NULL, "", "", "", "", "", "", "", "/", "?", NULL,
                                          NULL, NULL, NULL, NULL, NULL, NULL
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

/* THE NUMBERS-AND-CONTROLS LAYOUT — the other half of the keyboard, reached by
 * the numeric-pad key in the text layout's bottom-right corner and left by the
 * plain keyboard glyph in the same corner here.
 *
 *     |ESC| 7 | 8 | 9 |   | ↑ |   |
 *     |tab| 4 | 5 | 6 | ← |   | → |
 *     | ⌫ | 1 | 2 | 3 |   | ↓ |   |
 *     |⌨̸ | 0 | . | - |⌨ |  ⏎  |
 *
 * A 3x4 numeric pad down the middle; down the left the keys a terminal needs
 * and the letters have no room for — escape at the top corner where a terminal
 * user's hand expects it, tab under it, rubbing out below that; and to the
 * right of the pad the four cursor keys, set around an empty square the way a
 * keyboard with room for them sets them. Four rows rather than five, so the
 * keys come out taller than the letters'.
 *
 * THE ARROWS ARE A CROSS, NOT A ROW. Up and down have to be here — a terminal
 * with no way to walk back through its history is a terminal missing its most
 * used key — and four arrows in a line are read one at a time, where a cross is
 * read at a glance and hit without looking. The middle of the cross and its
 * four corners are HIDDEN keys: laid out, drawn as nothing, pressing nothing.
 * The empty square is what makes the shape legible, and the price of it is
 * narrower digits — which the pad can afford, being three columns where the
 * letter rows are ten.
 *
 * The bottom row is the text layout's, key for key: away in the corner, enter
 * in the corner opposite, and the way to the other layout just inside enter.
 * Nothing an operator has learned about where those are stops being true
 * because the middle changed. */
static const char * const default_kb_map_spec[] = {"ESC", "7", "8", "9", " ", LV_SYMBOL_UP, " ", "\n",
                                                   "tab", "4", "5", "6", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, "\n",
                                                   LV_SYMBOL_BACKSPACE, "1", "2", "3", " ", LV_SYMBOL_DOWN, " ", "\n",
                                                   kb_hide_lbl, "0", ".", "-", kb_abc_lbl, LV_SYMBOL_NEW_LINE, ""
                                                  };

/* Fifteen units to a row throughout: three for the left column, two for each of
 * the pad's three columns, two for each of the cross's three. The bottom row
 * spends the same fifteen differently and still keeps 0, . and - under 1, 2 and
 * 3, with the way back to the letters under the cross's left arm and enter
 * filling the corner. The arrows and the backspace repeat under a held finger
 * (no NO_REPEAT bit); ESC, tab and the two corner keys do not — none of them
 * means anything twice. */
static const lcd_keygrid_ctrl_t default_kb_ctrl_spec_map[] = {
    LCD_KEYS_CTRL_BUTTON_FLAGS | 3, LV_KB_BTN(2), LV_KB_BTN(2), LV_KB_BTN(2), LCD_KEYGRID_CTRL_HIDDEN | 2, LCD_KEYGRID_CTRL_CHECKED | 2, LCD_KEYGRID_CTRL_HIDDEN | 2,
    LCD_KEYS_CTRL_BUTTON_FLAGS | 3, LV_KB_BTN(2), LV_KB_BTN(2), LV_KB_BTN(2), LCD_KEYGRID_CTRL_CHECKED | 2, LCD_KEYGRID_CTRL_HIDDEN | 2, LCD_KEYGRID_CTRL_CHECKED | 2,
    LCD_KEYGRID_CTRL_CHECKED | 3, LV_KB_BTN(2), LV_KB_BTN(2), LV_KB_BTN(2), LCD_KEYGRID_CTRL_HIDDEN | 2, LCD_KEYGRID_CTRL_CHECKED | 2, LCD_KEYGRID_CTRL_HIDDEN | 2,
    LCD_KEYS_CTRL_BUTTON_FLAGS | LCD_KEYGRID_CTRL_STRUCK | 3, LV_KB_BTN(2), LV_KB_BTN(2), LV_KB_BTN(2), LCD_KEYS_CTRL_BUTTON_FLAGS | 2, LCD_KEYGRID_CTRL_CHECKED | 4
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

/* WHAT ELSE A KEY CAN MAKE — the accented letters, and the odd symbol that
 * belongs to a letter by name. Hold a key down and a chooser opens over these
 * (lcd_keys_alt_open); they are on no layout and reachable no other way.
 *
 * Keyed by the CAP and not by a button index, so a key carries its alternates
 * into every layout that prints it: the digits and the punctuation stand on both
 * maps and answer the same on either.
 *
 * ONE SET FOR A FAMILY OF LANGUAGES, NOT ONE PER LANGUAGE. What a European Latin
 * alphabet actually puts on a letter, in the order a hand hunting for one
 * sweeps: the accents first (acute, grave, circumflex, diaeresis, tilde), then
 * the ring, macron and ogonek, then the characters that are a letter in their
 * own right where they are used at all — æ œ ø ß ð þ ł. Between them they cover
 * French, German, Spanish, Portuguese, Italian, Dutch, Catalan, the Nordics,
 * Polish, Czech, Slovak, Slovene, Croatian, Hungarian, Romanian, Turkish and the
 * Baltics. This is not a claim to write those languages properly — there is one
 * keyboard here and its layout is a US one — but a name, a place or a borrowed
 * word in any of them can be typed without leaving it.
 *
 * The two that are not letters at all are here because the letter is how anyone
 * would look for them: µ under m, Ω under o. Everything is written in LOWER
 * case; a chooser opened with shift on capitalises what it shows and what it
 * types (lcd_keys_capital). Every character is inside the ranges the shipped
 * faces are subset to — Latin-1, Latin Extended-A, and the handful of named
 * codepoints in esp-idf/scripts/lcd-fonts.py. Adding one outside them types a
 * box, so add it there in the same breath. Under CONFIG_LCD_FONT_BITMAP, where
 * there is no engine and lcdFont() lands on a compiled bitmap, the accents are
 * there but Ω is not: the compiled faces carry Latin-1 and Latin Extended-A and
 * stop. */
static const struct {
    const char * cap;
    const char * alts;
} kb_alts[] = {
    { "A", "àáâäãåæā" },
    { "C", "çćč"      },
    { "D", "ďđð"      },
    { "E", "èéêëēę€"  },
    { "G", "ğĝģ"      },
    { "I", "ìíîïīį"   },
    { "L", "łľĺ"      },
    { "M", "µ"        },
    { "N", "ñńňņ"     },
    { "O", "òóôöõøœΩ" },
    { "R", "řŕ"       },
    { "S", "ßśšş"     },
    { "T", "ťţþ"      },
    { "U", "ùúûüūůű"  },
    { "W", "ŵ"        },
    { "Y", "ýÿ"       },
    { "Z", "žźż"      },
    { "0", "°"        },
    { "-", "–—"       },
    { ".", "…·"       },
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

void lcd_keys_arm_shift(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    /* A lock outranks it: caps is a state the operator chose and this is a
     * guess about a sentence. */
    if(keyboard->shift != LCD_KEYS_SHIFT_OFF) return;
    lcd_keys_set_shift(obj, LCD_KEYS_SHIFT_ONCE);
}

void lcd_keys_set_alt_font(lv_obj_t * obj, const lv_font_t * font)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    keyboard->alt_font = font;
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
    if(keyboard->mode == mode && keyboard->shift == shift &&
       !keyboard->fn && !keyboard->ctrl) return;

    keyboard->mode = mode;
    keyboard->shift = shift;
    keyboard->fn = 0;
    keyboard->ctrl = 0;
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
    const bool ctrl_at_press = keyboard->ctrl;

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
    if(txt == kb_ctrl_lbl) {
        lcd_keys_set_ctrl(obj, !ctrl_at_press);
        return;
    }

    /* THE TWO CORNERS, known by their cell and not their glyph — the away key
     * and the back-to-letters key are the same keyboard symbol, and only the
     * struck-through bit tells them apart on screen. */
    if(txt == kb_hide_lbl) {
        lv_result_t res = lv_obj_send_event(obj, LV_EVENT_CANCEL, NULL);
        if(res != LV_RESULT_OK) return;
        if(keyboard->ta) lv_obj_send_event(keyboard->ta, LV_EVENT_CANCEL, NULL);
        return;
    }
    if(txt == kb_num_lbl) {
        lcd_keys_set_mode(obj, LCD_KEYS_MODE_SPECIAL);
        return;
    }
    if(txt == kb_abc_lbl) {
        lcd_keys_set_mode(obj, LCD_KEYS_MODE_TEXT_LOWER);
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
        /*Shift turns rubbing out into deleting forward, as it does on a keyboard
         *with keys. The cap says nothing about it: the key is still backspace.*/
        else if(lv_strcmp(txt, LV_SYMBOL_BACKSPACE) == 0)
            key = shift_at_press != LCD_KEYS_SHIFT_OFF ? LV_KEY_DEL : LV_KEY_BACKSPACE;
        else if(lv_strcmp(txt, LV_SYMBOL_LEFT) == 0)      key = LV_KEY_LEFT;
        else if(lv_strcmp(txt, LV_SYMBOL_RIGHT) == 0)     key = LV_KEY_RIGHT;
        else if(lv_strcmp(txt, LV_SYMBOL_UP) == 0)        key = LV_KEY_UP;
        else if(lv_strcmp(txt, LV_SYMBOL_DOWN) == 0)      key = LV_KEY_DOWN;
        else if(lv_strcmp(txt, "ESC") == 0)               key = LV_KEY_ESC;
        else if(lv_strcmp(txt, "tab") == 0)               key = '\t';
        else {
            /*Every character these layouts can produce is one ASCII byte*/
            key = (uint8_t)lcd_keys_output(obj, txt, btn_id, shift_at_press != LCD_KEYS_SHIFT_OFF,
                                           fn_at_press, out, sizeof(out))[0];
        }
        /*ctrl is a BIT ON THE KEY, not a character of its own — lcd.h's
         *LCD_KEY_CTRL — so a sink that understands combinations gets one and a
         *sink that does not still gets the letter.*/
        if(ctrl_at_press) key |= LCD_KEY_CTRL;
        if(lv_obj_is_valid(keyboard->sink)) lv_obj_send_event(keyboard->sink, LV_EVENT_KEY, &key);
        lcd_keys_spend(obj, shift_at_press, fn_at_press, ctrl_at_press);
        /*The line is done. What that means for the keyboard is not ours to say*/
        if(key == LV_KEY_ENTER) lv_obj_send_event(obj, LV_EVENT_READY, NULL);
        return;
    }

    /*Add the characters to the text area if set*/
    if(keyboard->ta == NULL || !lv_obj_is_valid(keyboard->ta)) {
        lcd_keys_spend(obj, shift_at_press, fn_at_press, ctrl_at_press);
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
    else if(lv_strcmp(txt, LV_SYMBOL_UP) == 0) {
        lv_textarea_cursor_up(keyboard->ta);
    }
    else if(lv_strcmp(txt, LV_SYMBOL_DOWN) == 0) {
        lv_textarea_cursor_down(keyboard->ta);
    }
    else if(lv_strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        /*Shift makes it delete forward instead — the same key, doing what the
         *key beside it would do on a keyboard that had room for one.*/
        if(shift_at_press != LCD_KEYS_SHIFT_OFF) lv_textarea_delete_char_forward(keyboard->ta);
        else lv_textarea_delete_char(keyboard->ta);
    }
    else if(lv_strcmp(txt, "tab") == 0) {
        lv_textarea_add_char(keyboard->ta, '\t');
    }
    else if(lv_strcmp(txt, "ESC") == 0) {
        /*Nothing to type. A field is left by the away key or by Enter.*/
    }
    else if(ctrl_at_press) {
        /*A control combination is not text. It reaches a sink; a field ignores
         *it rather than inserting whatever the letter would have been.*/
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

    lcd_keys_spend(obj, shift_at_press, fn_at_press, ctrl_at_press);
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
    keyboard->ctrl       = 0;
    keyboard->popovers   = 0;
    keyboard->flick      = LCD_KEYS_FLICK_NONE;
    keyboard->flick_btn  = LCD_KEYGRID_BUTTON_NONE;
    keyboard->last_btn   = LCD_KEYGRID_BUTTON_NONE;
    keyboard->last_tick  = 0;
    keyboard->alt_pop    = NULL;
    keyboard->alt_font   = NULL;
    keyboard->alt_cnt    = 0;
    keyboard->alt_sel    = LCD_KEYS_ALT_NONE;

    lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(obj, lcd_keys_def_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* One handler for both gestures the press can turn into. The press picks the
     * key and the distance, each drag step may recognise the throw or move the
     * chooser's highlight, the hold opens the chooser, and either ending has to
     * clear whichever is standing. */
    lv_obj_add_event_cb(obj, lcd_keys_flick_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, lcd_keys_flick_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, lcd_keys_flick_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(obj, lcd_keys_flick_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, lcd_keys_flick_cb, LV_EVENT_PRESS_LOST, NULL);
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
    /* Whatever a chooser was standing over is about to stop existing. */
    lcd_keys_alt_close(obj, false);
    lcd_keygrid_set_map(obj, kb_map[keyboard->mode]);
    lcd_keys_update_ctrl_map(obj);
    /*After the map, which drops whatever marks the last one had*/
    lcd_keygrid_set_marks(obj, kb_mark_tl[keyboard->mode], kb_mark_tr[keyboard->mode]);
    lcd_keys_size_to_map(obj);
    /*And after the ctrl map, which has just put both modifier keys back to
     *their resting look whatever the modifiers are actually doing*/
    lcd_keys_set_shift(obj, (lcd_keys_shift_t)keyboard->shift);
    lcd_keys_set_fn(obj, keyboard->fn);
    lcd_keys_set_ctrl(obj, keyboard->ctrl);
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
static void lcd_keys_set_ctrl(lv_obj_t * obj, bool on)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    keyboard->ctrl = on;
    /* Blue when it is up, like the other two. It carries no corner marks of its
     * own — a control combination is the letter, not a different letter — so at
     * rest it is plain chrome rather than fn's tinted legend. */
    lcd_keys_dress(obj, kb_ctrl_lbl, on ? LCD_KEYGRID_CTRL_ACCENT
                                        : LCD_KEYGRID_CTRL_CHECKED);
}

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
static void lcd_keys_spend(lv_obj_t * obj, lcd_keys_shift_t shift_at_press,
                           bool fn_at_press, bool ctrl_at_press)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    if(shift_at_press == LCD_KEYS_SHIFT_ONCE && keyboard->shift == LCD_KEYS_SHIFT_ONCE) {
        lcd_keys_set_shift(obj, LCD_KEYS_SHIFT_OFF);
    }
    if(fn_at_press && keyboard->fn) lcd_keys_set_fn(obj, false);
    if(ctrl_at_press && keyboard->ctrl) lcd_keys_set_ctrl(obj, false);
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

/* ======================= THE LONG-PRESS CHOOSER =======================
 *
 * HELD DOWN, A KEY OPENS EVERYTHING ELSE IT CAN MAKE: what it types on a tap
 * first, then what shift makes of it, then what fn makes, then its accented
 * forms. The finger never lifts — it slides up onto the one it wants, which
 * highlights under it, and lifting there types it.
 *
 * BIG, AND WELL CLEAR OF THE KEYS. The characters are drawn at twice the legend
 * size (lcd_keys_set_alt_font), because the whole point of this is to tell ä
 * from å from ā, and at key size those are three of the same smudge under a
 * fingertip. The panel is centred across the screen and stands a whole
 * character's height above the top row of keys, over whatever the keyboard is
 * covering: away from the hand, nowhere near the keys it must not be confused
 * with, and above the line of whatever is being typed into — a keyboard is
 * raised under a field, and a panel sitting on that field hides the very thing
 * the character is going into. Past five characters it splits into two rows
 * within a character of each other in length, which keeps the panel roughly
 * square instead of a strip running off both edges of a 320 px screen.
 *
 * NOTHING IS CHOSEN UNTIL THE FINGER IS ON A CHARACTER, and lifting off the
 * panel types nothing at all — the chooser simply goes away. A hold nobody
 * meant is therefore free: no character, no keystroke to undo. It also means
 * the panel can sit where it is best seen rather than where it would have to be
 * for a default cell to fall under the finger.
 *
 * THE PRESS IS GIVEN UP AS THE CHOOSER OPENS, the same way and for the same
 * reason as a flick gives it up: the grid would otherwise type the key on the
 * release that picks a character. Dropping the grid's selection also ends the
 * long-press REPEAT before it starts, which is why holding a letter no longer
 * stutters it out while holding backspace still rubs out.
 *
 * A key with nothing but itself to offer never opens one — there would be a
 * single cell to choose from — so the modifiers, space, enter and the corner
 * keys are untouched, and backspace and the arrows keep repeating.
 */

/** Characters past which the panel is two rows rather than one. */
#define LCD_KEYS_ALT_ROW_MAX    5
/** How much bigger a cell is than the character in it. */
#define LCD_KEYS_ALT_CELL_NUM   5
#define LCD_KEYS_ALT_CELL_DEN   4
/** Clearance from the panel's edges to the screen's, in px. */
#define LCD_KEYS_ALT_MARGIN     4

/* Upper case, for the alternates and nothing else. Latin-1's small letters are
 * their capitals plus 0x20; Latin Extended-A is laid out in case PAIRS, and the
 * parity of those pairs flips twice along the block — which is why this is a
 * list of ranges and not one piece of arithmetic. A character with no capital
 * (ß, µ, Ω, the punctuation) comes back as it went in, and the caller then
 * copies the original bytes rather than re-encoding. */
static uint32_t lcd_keys_capital(uint32_t cp)
{
    if(cp >= 'a' && cp <= 'z')                       return cp - 0x20;
    if(cp >= 0x00E0 && cp <= 0x00FE && cp != 0x00F7) return cp - 0x20;          /* à … þ */
    if(cp == 0x00FF)                                 return 0x0178;             /* ÿ → Ÿ */
    if(cp >= 0x0100 && cp <= 0x0137)                 return cp & ~1u;           /* ā … ķ: even is the capital */
    if(cp >= 0x0139 && cp <= 0x0148)                 return (cp & 1) ? cp : cp - 1;  /* ĺ … ň: odd is */
    if(cp >= 0x014A && cp <= 0x0177)                 return cp & ~1u;           /* ŋ … ŷ: even again */
    if(cp >= 0x0179 && cp <= 0x017E)                 return (cp & 1) ? cp : cp - 1;  /* ź … ž: odd */
    return cp;
}

/* One codepoint as UTF-8. Only ever called on what lcd_keys_capital changed, so
 * only the one- and two-byte forms can arrive. */
static void lcd_keys_utf8(uint32_t cp, char * out)
{
    if(cp < 0x80) {
        out[0] = (char)cp;
        out[1] = '\0';
    }
    else {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0';
    }
}

/* Put a character where the keyboard is pointed. A sink takes a KEY, and a key
 * is a codepoint, so the UTF-8 is decoded here — that is what carries an
 * accented letter to a terminal as the letter rather than as the first byte of
 * one. A field takes the bytes as they are. */
static void lcd_keys_emit(lv_obj_t * obj, const char * utf8)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    if(!utf8 || !utf8[0]) return;

    if(keyboard->sink && lv_obj_is_valid(keyboard->sink)) {
        uint32_t i = 0;
        uint32_t key = lv_text_encoded_next(utf8, &i);
        if(keyboard->ctrl) key |= LCD_KEY_CTRL;
        lv_obj_send_event(keyboard->sink, LV_EVENT_KEY, &key);
    }
    else if(keyboard->ta && lv_obj_is_valid(keyboard->ta)) {
        lv_textarea_add_text(keyboard->ta, utf8);
    }
}

/* Add a cell, unless the same character is already in the row. The duplicates
 * are the ordinary case and not an edge one: a letter's shift alternate IS its
 * capital, and with shift already on that is the cap the tap would type. */
static void lcd_keys_alt_add(lcd_keys_t * keyboard, const char * s)
{
    if(!s || !s[0] || keyboard->alt_cnt >= LCD_KEYS_ALT_MAX) return;
    if(lv_strlen(s) >= sizeof(keyboard->alt[0].txt)) return;
    uint32_t i;
    for(i = 0; i < keyboard->alt_cnt; i++)
        if(lv_strcmp(keyboard->alt[i].txt, s) == 0) return;

    lv_strlcpy(keyboard->alt[keyboard->alt_cnt].txt, s, sizeof(keyboard->alt[0].txt));
    keyboard->alt[keyboard->alt_cnt].label = NULL;
    keyboard->alt_cnt++;
}

/* A letter of the alphabet, as opposed to a digit, a punctuation mark or a
 * named key. What the two cases of a letter are is settled without asking. */
static bool lcd_keys_is_letter(const char * cap)
{
    return cap[0] >= 'A' && cap[0] <= 'Z' && cap[1] == '\0';
}

/* The panel's contents, in the order they are shown. */
static void lcd_keys_alt_fill(lv_obj_t * obj, uint32_t btn_id, const char * cap)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    const bool up = keyboard->shift != LCD_KEYS_SHIFT_OFF;
    char buf[8];
    uint32_t i;

    keyboard->alt_cnt = 0;
    if(lcd_keys_is_letter(cap)) {
        /* A LETTER OFFERS NEITHER OF ITS CASES. Both are already one tap away —
         * the key types the small one, shift types the capital — so those two
         * cells would be the two things the operator can do without opening
         * anything at all, standing in front of the ones they cannot. Its fn
         * alternate is not one of those, so it stays: the mark itself rather
         * than lcd_keys_output, which answers with the letter for a key that
         * has no fn mark. */
        const char * const * tl = kb_mark_tl[keyboard->mode];
        if(tl && tl[btn_id] && tl[btn_id][0]) lcd_keys_alt_add(keyboard, tl[btn_id]);
    }
    else {
        lcd_keys_alt_add(keyboard, lcd_keys_output(obj, cap, btn_id, up, false, buf, sizeof(buf)));
        lcd_keys_alt_add(keyboard, lcd_keys_output(obj, cap, btn_id, true, false, buf, sizeof(buf)));
        lcd_keys_alt_add(keyboard, lcd_keys_output(obj, cap, btn_id, false, true, buf, sizeof(buf)));
    }

    const char * alts = NULL;
    for(i = 0; i < sizeof(kb_alts) / sizeof(kb_alts[0]); i++)
        if(lv_strcmp(kb_alts[i].cap, cap) == 0) { alts = kb_alts[i].alts; break; }
    if(!alts) return;

    uint32_t at = 0;
    while(alts[at]) {
        uint32_t next = at;
        uint32_t cp   = lv_text_encoded_next(alts, &next);
        uint32_t cap_cp = up ? lcd_keys_capital(cp) : cp;
        if(cap_cp != cp) {
            lcd_keys_utf8(cap_cp, buf);
        }
        else {
            lv_memcpy(buf, alts + at, next - at);
            buf[next - at] = '\0';
        }
        lcd_keys_alt_add(keyboard, buf);
        at = next;
    }
}

/* Dress cell `i` as the one the finger is on, or as one it is not. */
static void lcd_keys_alt_mark(lcd_keys_t * keyboard, uint8_t i, bool on)
{
    lv_obj_t * cell = i < keyboard->alt_cnt ? keyboard->alt[i].label : NULL;
    if(!cell) return;
    lv_obj_set_style_bg_opa(cell, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(cell, on ? lv_color_white() : lv_color_hex(0xe6ebf0), 0);
}

/* Move the highlight to the cell the finger is on, or take it off everything
 * when the finger is not on one. The cells are asked where they are rather than
 * worked out from an origin and a pitch: there are at most twelve of them, and
 * two rows of unequal length have no single pitch to divide by. */
static void lcd_keys_alt_track(lv_obj_t * obj, int32_t x, int32_t y)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    if(!keyboard->alt_pop) return;

    uint8_t hit = LCD_KEYS_ALT_NONE;
    uint8_t i;
    for(i = 0; i < keyboard->alt_cnt; i++) {
        lv_area_t a;
        if(!keyboard->alt[i].label) continue;
        lv_obj_get_coords(keyboard->alt[i].label, &a);
        if(x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) { hit = i; break; }
    }
    if(hit == keyboard->alt_sel) return;

    lcd_keys_alt_mark(keyboard, keyboard->alt_sel, false);
    keyboard->alt_sel = hit;
    lcd_keys_alt_mark(keyboard, keyboard->alt_sel, true);
}

/* Take the panel down. `commit` types what the finger was on — and there may be
 * nothing under it, which is a release that types nothing. */
static void lcd_keys_alt_close(lv_obj_t * obj, bool commit)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    if(!keyboard->alt_pop) return;

    lv_obj_delete(keyboard->alt_pop);
    keyboard->alt_pop = NULL;
    for(uint8_t i = 0; i < keyboard->alt_cnt; i++) keyboard->alt[i].label = NULL;

    if(commit && keyboard->alt_sel < keyboard->alt_cnt) {
        lcd_keys_emit(obj, keyboard->alt[keyboard->alt_sel].txt);
        /* A pick spends what was armed, exactly as a press does — the character
         * it produced was chosen with the modifiers in the state they are in. */
        lcd_keys_spend(obj, (lcd_keys_shift_t)keyboard->shift, keyboard->fn, keyboard->ctrl);
        lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
    }
    keyboard->alt_cnt = 0;
    keyboard->alt_sel = LCD_KEYS_ALT_NONE;
}

/**
 * Open the chooser for a key being held.
 * @param obj     the keyboard
 * @param btn_id  the key being held
 */
static void lcd_keys_alt_open(lv_obj_t * obj, uint32_t btn_id)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    lcd_keygrid_t * btnm  = (lcd_keygrid_t *)obj;
    if(keyboard->alt_pop) return;

    const char * cap = lcd_keygrid_get_button_text(obj, btn_id);
    if(!cap) return;

    /*ONE CHARACTER ON THE CAP, or the key is not a character key at all: "ESC"
     *and "tab" and "+/-" are names, and a chooser offering to type the name is
     *nonsense. A symbol legend passes this — it is one codepoint — and falls out
     *further down for having nothing to offer but itself.*/
    uint32_t at = 0;
    lv_text_encoded_next(cap, &at);
    if(cap[at] != '\0') return;

    /*Nothing to choose between: a key that offers only what it already types.
     *A letter does not offer that, so one cell is a whole chooser for it — µ
     *under m is exactly that case.*/
    lcd_keys_alt_fill(obj, btn_id, cap);
    if(keyboard->alt_cnt < (lcd_keys_is_letter(cap) ? 1 : 2)) {
        keyboard->alt_cnt = 0;
        return;
    }

    /*The characters, and the cell that holds one, come off the chooser's own
     *face — a big one, set per open by lcd_keyboard.cpp. Falling back to the
     *legend font only keeps a keyboard nobody has dressed usable.*/
    const lv_font_t * font = keyboard->alt_font ? keyboard->alt_font
                                                : lv_obj_get_style_text_font(obj, LV_PART_ITEMS);
    const int32_t fh = lv_font_get_line_height(font);
    const int32_t scr = lv_display_get_horizontal_resolution(lv_obj_get_display(obj));

    /*One row up to five characters, two beyond that, the fuller row on top and
     *never more than one character longer than the other.*/
    const int32_t rows = keyboard->alt_cnt > LCD_KEYS_ALT_ROW_MAX ? 2 : 1;
    const int32_t per  = ((int32_t)keyboard->alt_cnt + rows - 1) / rows;

    /*A cell is the character with room around it — then no wider than its share
     *of the screen, and no taller than its share of what there IS above the
     *keys. On a 240 px panel under a five-row keyboard that is about a hundred
     *pixels for the panel and its clearance, so the cells give way rather than
     *the clearance: a panel crowding the keys is one a finger picks from by
     *accident on its way up.*/
    /*A whole character's height of clearance, which is what lifts the panel off
     *the entry field the keyboard was raised under: one line of text and its
     *padding is about that, and the field is the last thing that should be
     *covered while it is being typed into.*/
    lv_area_t obj_area;
    lv_obj_get_coords(obj, &obj_area);
    const int32_t gap  = fh;
    const int32_t room = obj_area.y1 - gap;

    int32_t cell_px = fh * LCD_KEYS_ALT_CELL_NUM / LCD_KEYS_ALT_CELL_DEN;
    if(cell_px * per > scr - 2 * LCD_KEYS_ALT_MARGIN)
        cell_px = (scr - 2 * LCD_KEYS_ALT_MARGIN) / per;
    if(cell_px * rows > room) cell_px = room / rows;
    if(cell_px < 1) { keyboard->alt_cnt = 0; return; }   /*no screen above the keys at all*/

    const int32_t w = cell_px * per;
    const int32_t h = cell_px * rows;

    /*Centred across the screen and standing clear of the keys: the hand is at
     *the keyboard, so the choosing happens where the hand is not.*/
    int32_t x0 = (scr - w) / 2;
    int32_t y0 = obj_area.y1 - gap - h;
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;

    /*On the top layer: the panel stands over the keyboard and over whatever the
     *keyboard is itself standing over.*/
    lv_obj_t * pop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pop);
    lv_obj_remove_flag(pop, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(pop, w, h);
    lv_obj_set_pos(pop, x0, y0);
    lv_obj_set_style_bg_color(pop, lv_color_hex(0x11161c), 0);
    lv_obj_set_style_bg_opa(pop, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pop, lv_color_hex(0x3a4450), 0);
    lv_obj_set_style_border_width(pop, 1, 0);
    lv_obj_set_style_radius(pop, 6, 0);
    lv_obj_set_style_pad_all(pop, 0, 0);
    lv_obj_set_style_text_font(pop, font, 0);
    lv_obj_move_foreground(pop);

    /*A cell is a box with the character centred in it: the box carries the
     *highlight, and the text colour it is dressed in is inherited by the label
     *inside it. A short second row is centred under the first.*/
    uint32_t i;
    for(i = 0; i < keyboard->alt_cnt; i++) {
        const int32_t row  = (int32_t)i / per;
        const int32_t col  = (int32_t)i % per;
        const int32_t wide = (row == rows - 1) ? (int32_t)keyboard->alt_cnt - row * per : per;

        lv_obj_t * cell = lv_obj_create(pop);
        lv_obj_remove_style_all(cell);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        /*Where the room above the keys squeezed the cells below the character's
         *own height, it is the box that is short — the character still draws.*/
        lv_obj_add_flag(cell, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_set_size(cell, cell_px, cell_px);
        lv_obj_set_pos(cell, (w - wide * cell_px) / 2 + col * cell_px, row * cell_px);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x4a90d9), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_text_color(cell, lv_color_hex(0xe6ebf0), 0);

        lv_obj_t * lbl = lv_label_create(cell);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_text(lbl, keyboard->alt[i].txt);
        lv_obj_center(lbl);

        keyboard->alt[i].label = cell;
    }

    keyboard->alt_pop = pop;
    keyboard->alt_sel = LCD_KEYS_ALT_NONE;   /*until the finger is on one*/

    /*The press is spent on the chooser now: no key on the release, and no
     *long-press repeat while the finger hunts.*/
    btnm->pressed = 0;
    lcd_keygrid_set_selected_button(obj, LCD_KEYGRID_BUTTON_NONE);
}

/* ============================ THE FLICK ============================
 *
 * A KEY CAN BE PRESSED, OR IT CAN BE THROWN. Every cap already prints what its
 * modifiers make of it, small, in its two top corners; a flick reaches those
 * without arming a modifier first. Thrown UPWARDS — anywhere in the quadrant
 * either side of straight up — a key gives its top-RIGHT mark, which is what
 * shift makes. Thrown any other way it gives its top-LEFT mark, which is what
 * fn makes. Up is the direction a thumb can aim at without looking, so it goes
 * to the marks people reach for most.
 *
 * ONLY CAPS FLICK. A key with neither mark is not a cap at all — the modifiers,
 * space, enter, the corner keys — and throwing one does nothing, so those keys
 * keep working exactly as before while a finger is being clumsy near them.
 *
 * THE KEYPRESS IS GIVEN UP MID-THROW, not at the release. LVGL runs a widget's
 * own handler BETWEEN a user callback's pre- and post-passes and
 * lv_obj_add_event_cb registers the post pass, so by the time this file saw the
 * release the grid would already have typed the letter. Dropping the grid's
 * selection the moment the throw is recognised makes the two outcomes exclusive
 * by construction rather than by agreement.
 */

/**
 * Follow a press, and read a throw out of it when it lifts.
 * @param e the press/drag/release event on the keyboard
 */
static void lcd_keys_flick_cb(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_current_target(e);
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    lcd_keygrid_t * btnm = (lcd_keygrid_t *)obj;
    const lv_event_code_t code = lv_event_get_code(e);

    lv_indev_t * indev = lv_event_get_indev(e);
    if(indev == NULL || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;

    if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        /*A chooser owns the whole press once it is up: the release picks from it
         *and the throw the finger made getting there means nothing.*/
        if(keyboard->alt_pop) {
            lcd_keys_alt_close(obj, code == LV_EVENT_RELEASED);
            keyboard->flick = LCD_KEYS_FLICK_NONE;
            keyboard->flick_btn = LCD_KEYGRID_BUTTON_NONE;
            return;
        }
        /*A lost press went somewhere else; only a real release types.*/
        if(keyboard->flick != LCD_KEYS_FLICK_NONE && code == LV_EVENT_RELEASED) {
            const bool up = keyboard->flick == LCD_KEYS_FLICK_SHIFT;
            const char * txt = lcd_keygrid_get_button_text(obj, keyboard->flick_btn);
            char out[8];
            if(txt) {
                const char * put = lcd_keys_output(obj, txt, keyboard->flick_btn,
                                                   up, !up, out, sizeof(out));
                lcd_keys_emit(obj, put);
                /*A throw spends what was armed, exactly as a press does — it is
                 *a keystroke, it merely chose its own modifier.*/
                lcd_keys_spend(obj, (lcd_keys_shift_t)keyboard->shift,
                               keyboard->fn, keyboard->ctrl);
                lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
            }
        }
        keyboard->flick = LCD_KEYS_FLICK_NONE;
        keyboard->flick_btn = LCD_KEYGRID_BUTTON_NONE;
        return;
    }

    if(code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
       code != LV_EVENT_LONG_PRESSED) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    /*Held still on a key: everything else that key can make, over it. Not after
     *a throw has already been read out of this press — the finger has left.*/
    if(code == LV_EVENT_LONG_PRESSED) {
        if(keyboard->alt_pop || keyboard->flick != LCD_KEYS_FLICK_NONE) return;
        const uint32_t btn = btnm->btn_id_sel;
        if(btn == LCD_KEYGRID_BUTTON_NONE || btn >= btnm->btn_cnt) return;
        lcd_keys_alt_open(obj, btn);
        return;
    }

    /*While one is up the finger is picking from it, not typing and not
     *throwing.*/
    if(keyboard->alt_pop) {
        if(code == LV_EVENT_PRESSING) { lcd_keys_alt_track(obj, p.x, p.y); return; }
        /*A fresh press with one still standing: the press that opened it ended
         *somewhere this widget never heard about (the keys hidden under the
         *finger, say). Drop it and let this press be an ordinary one.*/
        lcd_keys_alt_close(obj, false);
    }

    if(code == LV_EVENT_PRESSED) {
        keyboard->flick = LCD_KEYS_FLICK_NONE;
        keyboard->flick_btn = LCD_KEYGRID_BUTTON_NONE;
        keyboard->flick_x = p.x;
        keyboard->flick_y = p.y;

        const uint32_t btn = btnm->btn_id_sel;
        if(btn == LCD_KEYGRID_BUTTON_NONE || btn >= btnm->btn_cnt) return;

        /*Only a cap can be thrown: a key with no corner marks has nothing a
         *throw could ask for.*/
        const char * const * tl = kb_mark_tl[keyboard->mode];
        const char * const * tr = kb_mark_tr[keyboard->mode];
        const bool cap = (tl && tl[btn]) || (tr && tr[btn]);
        if(!cap) return;

        const lv_area_t * a = &btnm->button_areas[btn];
        const int32_t h = a->y2 - a->y1 + 1;
        keyboard->flick_min = (h * LCD_KEYS_FLICK_NUM) / LCD_KEYS_FLICK_DEN;
        keyboard->flick_btn = btn;
        return;
    }

    if(keyboard->flick != LCD_KEYS_FLICK_NONE) return;
    if(keyboard->flick_btn == LCD_KEYGRID_BUTTON_NONE) return;

    const int32_t dx = p.x - keyboard->flick_x;
    const int32_t dy = p.y - keyboard->flick_y;
    const int32_t min = keyboard->flick_min;
    if(dx * dx + dy * dy < min * min) return;

    /*Up is the quadrant forty-five degrees either side of straight up, which is
     *exactly "went up further than it went sideways".*/
    keyboard->flick = (dy < 0 && -dy > (dx < 0 ? -dx : dx)) ? LCD_KEYS_FLICK_SHIFT
                                                            : LCD_KEYS_FLICK_FN;
    btnm->pressed = 0;   /*before the deselect, whose invalidate repaints it*/
    lcd_keygrid_set_selected_button(obj, LCD_KEYGRID_BUTTON_NONE);
}

/**
 * Update the control map for the current mode
 * @param obj pointer to a keyboard object
 */
static void lcd_keys_update_ctrl_map(lv_obj_t * obj)
{
    lcd_keys_t * keyboard = (lcd_keys_t *)obj;
    lcd_keygrid_t * btnm = (lcd_keygrid_t *)obj;

    /*Always a copy: two things are forced on top of whatever the map asked for*/
    lcd_keygrid_ctrl_t * ctrl_map = lv_malloc(btnm->btn_cnt * sizeof(lcd_keygrid_ctrl_t));
    if(ctrl_map == NULL) return;
    lv_memcpy(ctrl_map, kb_ctrl[keyboard->mode], sizeof(lcd_keygrid_ctrl_t) * btnm->btn_cnt);

    uint32_t i;
    for(i = 0; i < btnm->btn_cnt; i++) {
        /* EVERY KEY COMMITS ON RELEASE, NEVER ON PRESS.
         *
         * The grid types a key the moment it goes down unless the key is marked
         * CLICK_TRIG or POPOVER — and popovers are off here, so without this
         * every letter landed the instant a finger touched it. That is wrong
         * twice over: a press may still turn out to be a FLICK, and by the time
         * the finger has moved far enough to say so the letter would already
         * have been typed; and a touch key that cannot be escaped by sliding off
         * it is a touch key with no undo. */
        ctrl_map[i] |= LCD_KEYGRID_CTRL_CLICK_TRIG;

        if(!keyboard->popovers) ctrl_map[i] &= (~LCD_KEYGRID_CTRL_POPOVER);
    }

    lcd_keygrid_set_ctrl_map(obj, ctrl_map);
    lv_free(ctrl_map);
}
