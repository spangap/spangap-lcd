/**
 * lcd_keyboard.cpp — the panel's own keyboard, for a device that has no other.
 *
 * A text field on a touch-only board is not something anyone can answer: there
 * is no key to press. So a tap on one raises this — the keys (lcd_keys) across
 * the bottom of the screen, TYPING STRAIGHT INTO THE FIELD. There is nothing
 * between: no copy of the field, no ✓ that commits, nothing to write back. What
 * an app gets from this keyboard is what it would get from a keyboard with
 * keys, character by character as they are pressed, and an app therefore needs
 * to know nothing about it.
 *
 * THE SCREEN COMES UP TO MEET IT. The keys stand over the bottom of whatever is
 * behind them, so the thing being typed into is lifted clear: the field — or,
 * for a terminal, the cursor's own row — ends up just above the top edge of the
 * keys, and drops back when they go. What moves is the field's own top-level
 * object, the app's layer or the dialog it sits in, translated rather than
 * repositioned so that nothing it is doing to itself is disturbed. The
 * home-bar gesture stands down for the duration: its strip is under the keys,
 * and lifting the app out from under a keyboard is not a thing anyone means.
 *
 * The predicate every caller asks is the NEGATIVE one, `lcdKeyboardOnScreen()`
 * — "does typing have to come off the panel?" — not "is there a keyboard?".
 * A board with keys (the T-Deck) answers false and this module never appears;
 * so will a device that grows a keyboard at runtime, and the call sites do not
 * change when it does.
 *
 * TWO WAYS IN. `lcdKeyboardOpen(ta)` binds a text area and types into it.
 * `lcdKeyboardOpenKeys(sink, anchor)` binds anything at all and delivers plain
 * key events to it, which is what a terminal needs — and there Enter ends the
 * line and puts the keyboard away, since a terminal has no other way to say it
 * is done. Enter in a one-line field does the same and fires the field's
 * LV_EVENT_READY, which is the Enter a device with keys would have sent; in a
 * multi-line field it is a newline and the keyboard stays — unless that field
 * submits on Enter (lcd_input_box's own switch), which is the one-line bargain
 * made by a field that is multi-line only so that it can grow.
 *
 * The crossed-out keyboard key puts it away at any time. Being a tracked dialog
 * (lcdModalTrack) it also goes the same way for anything else that clears the
 * panel — the Home button on its way to the launcher, an app tearing down — so
 * navigation never happens behind it. What has been typed stays typed: it went
 * into the field as it was pressed, and closing a keyboard has never been how a
 * device with keys discards anything.
 *
 * Lcd task only, and one at a time — a second field asking while one is up
 * takes the keyboard over rather than stacking another.
 */
#include "lcd.h"
#include "lcd_internal.h"
#include "lcd_keys.h"       /* our own keyboard widget (a fork of lv_keyboard) */

#include <cstring>
#include <string>

namespace {

/* BUILT ONCE, PARKED, NEVER REBUILT.
 *
 * The keys are made on the first open and then kept for the life of the device,
 * hidden between uses: building forty-odd keys — parsing the map, allocating
 * their areas, applying the look — is work that belongs to the first open only,
 * and an operator tapping a field wants the keyboard NOW. What each open still
 * does is what actually differs: point it at this field, take on that field's
 * shape, lift the screen off it, show it. */
struct {
    lv_obj_t* keys;      /* lcd_keys — the keyboard, and all of it */
    lv_obj_t* target;    /* the field, or the sink, being typed into */
    lv_obj_t* anchor;    /* what must stay clear of the keys (a caret, a field) */
    lv_obj_t* lifted;    /* the top-level object we translated, to put back */
    int       lift;      /* by how much, in px */
    bool      up;
} s_kb;

void kbTargetGoneCb(lv_event_t* e);

/* The top-level object `o` belongs to: the child of the screen, or of the layer
 * a dialog was put on. That is the thing to move — moving the screen itself
 * would take the keyboard with it, and moving the field alone would tear it out
 * of the form it belongs to. */
lv_obj_t* topLevelOf(lv_obj_t* o) {
    lv_obj_t* top = o;
    for (lv_obj_t* p = lv_obj_get_parent(top); p && lv_obj_get_parent(p); p = lv_obj_get_parent(top))
        top = p;
    return top;
}

/* Bring the anchor clear of the keys, or let it back down. Reads where the
 * anchor IS — translation and all — and corrects by the difference, so the same
 * call serves the first lift and every re-check after it (a field that grows as
 * it is typed into walks its own caret down towards the keys). */
void kbSettleLift(void) {
    if (!s_kb.lifted || !lv_obj_is_valid(s_kb.lifted)) return;
    if (!s_kb.anchor || !lv_obj_is_valid(s_kb.anchor)) return;

    lv_obj_update_layout(s_kb.lifted);
    lv_obj_update_layout(s_kb.keys);
    lv_area_t a, k;
    lv_obj_get_coords(s_kb.anchor, &a);
    lv_obj_get_coords(s_kb.keys, &k);

    int want = k.y1 - lcdPx(4);          /* just above the top edge of the keys */
    int lift = s_kb.lift + (a.y2 - want);
    if (lift < 0) lift = 0;              /* never pull the screen down past home */
    if (lift == s_kb.lift) return;

    s_kb.lift = lift;
    lv_obj_set_style_translate_y(s_kb.lifted, -lift, 0);
}

void kbDrop(void) {
    if (s_kb.lifted && lv_obj_is_valid(s_kb.lifted))
        lv_obj_set_style_translate_y(s_kb.lifted, 0, 0);
    s_kb.lifted = nullptr;
    s_kb.anchor = nullptr;
    s_kb.lift   = 0;
}

/* Let go of the field: the keys stop pointing at it and it stops telling us it
 * is going. Typing into a field an app has deleted out from under an open
 * keyboard is the one way this can take the device down, so the binding is
 * dropped from BOTH ends. */
void kbUnbind(void) {
    if (s_kb.target && lv_obj_is_valid(s_kb.target))
        lv_obj_remove_event_cb(s_kb.target, kbTargetGoneCb);
    s_kb.target = nullptr;
    lcd_keys_set_textarea(s_kb.keys, nullptr);
    lcd_keys_set_key_sink(s_kb.keys, nullptr);
}

void kbClose(void) {
    if (!s_kb.up) return;
    s_kb.up = false;
    kbUnbind();
    kbDrop();
    /* Parked, not destroyed — and untracked by hand, since the escape list only
     * forgets an overlay when it is deleted. */
    lcdModalUntrack(s_kb.keys);
    lv_obj_add_flag(s_kb.keys, LV_OBJ_FLAG_HIDDEN);
    lcdShellHomebarSuppress(false);
}

/* READY is "this entry is finished" — Enter in a one-line field, Enter at a
 * terminal — and CANCEL is the crossed-out keyboard key. Both put it away; what
 * either MEANS to the app has already been sent by lcd_keys. */
void kbEvent(lv_event_t*) { kbClose(); }

void kbEscape(void*) { kbClose(); }

/* The field is being deleted — an app rebuilding its screen with the keyboard
 * still up. Take the keyboard down with it rather than hold a dead pointer. */
void kbTargetGoneCb(lv_event_t*) { kbClose(); }

void kbTapCb(lv_event_t* e) { lcdKeyboardOpen(lv_event_get_target_obj(e)); }

/* A key went in, so the field may have grown or its caret moved down towards
 * the keys: settle the lift again. Hung on the keyboard rather than on the
 * field, which would collect a callback per open. */
void kbTypedCb(lv_event_t*) { if (s_kb.up) kbSettleLift(); }

/* Digits (and what dots into them) only → the numeric pad, which is four times
 * the key for a field that can hold nothing else. */
bool numericOnly(const char* accepted) {
    return accepted && *accepted && strspn(accepted, "0123456789.-+") == strlen(accepted);
}

/* Its face. Re-applied per open because a font can be freed and rebuilt under us
 * (lcdFontsReset), and a parked widget must never be the thing holding the stale
 * pointer. Three legends on one key, and only ~23 px of key: the cap, then the
 * two corner marks a single size down. */
void kbFace(void) {
    lv_obj_set_style_text_font(s_kb.keys, lcdFont(LcdFace::UI, lcdPx(12)), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_kb.keys, lcdFont(LcdFace::UI, lcdPx(10)), LV_PART_INDICATOR);
    lv_obj_set_style_text_font(s_kb.keys, lcdFont(LcdFace::UI, lcdPx(10)), LV_PART_CUSTOM_FIRST);
    /* The long-press chooser, at twice the legend. It is shown one character at
     * a time with room around it, and it exists to be READ: ä, å and ā are one
     * smudge at 12 px under a finger. */
    lcd_keys_set_alt_font(s_kb.keys, lcdFont(LcdFace::UI, lcdPx(24)));
}

/* The one-time build. Everything here is true of every field. */
void kbBuild(void) {
    lv_obj_t* kb = lcd_keys_create(lv_layer_top());
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);      /* parked until a field asks */
    lv_obj_add_event_cb(kb, kbEvent, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(kb, kbEvent, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_event_cb(kb, kbTypedCb, LV_EVENT_VALUE_CHANGED, nullptr);
    s_kb.keys = kb;
}

/* Both ways in end here: bind, dress, raise, lift. */
void kbOpen(lv_obj_t* target, lv_obj_t* anchor, bool asSink) {
    if (!target || !lv_obj_is_valid(target)) return;
    if (!lcdKeyboardOnScreen()) return;
    if (s_kb.up && s_kb.target == target) return;   /* already typing into it */
    if (!s_kb.keys) kbBuild();
    if (s_kb.up) { kbUnbind(); kbDrop(); }          /* moving to another field */

    kbFace();

    if (asSink) {
        lcd_keys_set_key_sink(s_kb.keys, target);
        lcd_keys_set_mode(s_kb.keys, LCD_KEYS_MODE_TEXT_LOWER);
    } else {
        lcd_keys_set_key_sink(s_kb.keys, nullptr);
        lcd_keys_set_textarea(s_kb.keys, target);
        /* What the field would not accept typed into it directly it does not
         * accept from here either — the field itself refuses it, exactly as it
         * would from a keyboard with keys. All this open needs from its rules is
         * which layout suits them. */
        const char* accepted = lv_textarea_get_accepted_chars(target);
        const bool numeric = numericOnly(accepted);
        lcd_keys_set_mode(s_kb.keys, numeric ? LCD_KEYS_MODE_NUMBER
                                             : LCD_KEYS_MODE_TEXT_LOWER);

        /* A TEXT FIELD OPENED EMPTY STARTS A SENTENCE, so shift is armed for
         * its first letter — and the keyboard SAYS so, shift standing blue,
         * where a field that quietly upper-cased the letter could only be
         * argued with by typing it and going back. One tap refuses it. */
        const char* cur = lv_textarea_get_text(target);
        if(!numeric && (!cur || !cur[0])) lcd_keys_arm_shift(s_kb.keys);
    }

    s_kb.target = target;
    lv_obj_add_event_cb(target, kbTargetGoneCb, LV_EVENT_DELETE, nullptr);
    s_kb.anchor = anchor && lv_obj_is_valid(anchor) ? anchor : target;
    s_kb.up     = true;

    lv_obj_move_foreground(s_kb.keys);   /* above anything raised since */
    lv_obj_remove_flag(s_kb.keys, LV_OBJ_FLAG_HIDDEN);
    lcdModalTrack(s_kb.keys, kbEscape);
    lcdShellHomebarSuppress(true);       /* its strip is under the keys now */

    s_kb.lifted = topLevelOf(s_kb.anchor);
    s_kb.lift   = 0;
    kbSettleLift();
}

}  // namespace

/* BUILT AND DRAWN ONCE AT BOOT, WHILE THE SCREEN IS STILL DARK.
 *
 * Parking the widget was never the whole cost of the first open. Making forty
 * keys is cheap; RASTERISING THEIR LEGENDS is not — the font engine renders a
 * glyph the first time it is asked for one and caches it after, so the first
 * keyboard to appear pays for the whole alphabet at three sizes and every one
 * after it pays nothing. That is why the second open always felt instant and the
 * first did not, and it cannot be fixed by building the widget alone: a hidden
 * object is never drawn, so it never asks for a glyph.
 *
 * So this shows the keys, forces one refresh, and hides them again. It runs from
 * the lcd task's own bring-up, inside the window where the backlight is still
 * held down for the boot reveal (lcd_lvgl.cpp) — the keyboard is drawn onto
 * glass nobody is looking at yet, and the cost lands where there is already
 * nothing to wait for.
 *
 * A board with keys of its own never calls this: lcdKeyboardOnScreen() is false
 * and there is nothing to warm. */
void lcdKeyboardPreload(void) {
    if (!lcdKeyboardOnScreen()) return;
    if (s_kb.keys) return;

    kbBuild();
    kbFace();

    lv_obj_remove_flag(s_kb.keys, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_kb.keys);
    lv_refr_now(nullptr);
    lv_obj_add_flag(s_kb.keys, LV_OBJ_FLAG_HIDDEN);
}

bool lcdKeyboardOnScreen(void) { return !lcdHasKeyboard(); }

bool lcdKeyboardIsOpen(void) { return s_kb.up; }

lv_obj_t* lcdKeyboardTarget(void) { return s_kb.up ? s_kb.target : nullptr; }

/* HOW MUCH SCREEN `o` HAS TO GROW INTO — from the top of the layer it belongs to
 * down to the top edge of the keys.
 *
 * Answered whether or not the keys are up, and that is the point: on a board
 * that types over the panel they are always about to be, and a field that sized
 * itself to the whole screen while they were down would be too tall the instant
 * they arrived — with no second chance to shrink, because the lift that puts its
 * bottom above the keys is what pushes its top off the top. So the room is the
 * room WITH the keyboard, always. The parked widget answers for the closed case:
 * it is built and laid out at boot (lcdKeyboardPreload), and it is the text
 * layout — the taller of the two — so the number is honest for the shorter one
 * too. A board with keys of its own has no keys widget and gets the screen.
 *
 * The ceiling is the same top-level object the lift moves, taken at REST: a
 * translated object reports translated coordinates, so the lift is added back —
 * the question is how much room the layer has, not where it has been pushed to.
 * It is never taken below the status bar, though: a layer that starts further
 * down the screen is a DIALOG, which is lifted bodily and may hang off the top
 * if it must, and reading its own top as a ceiling would squeeze the field
 * inside it to a line or two. So the ceiling is the top of the screen, or the
 * foot of the status bar where one could be standing over it. Off it comes the
 * same clearance the lift leaves above the keys, so a thing grown to exactly
 * this height sits between the two edges and touches neither. */
int lcdKeyboardRoomFor(lv_obj_t* o) {
    const int screenH = lcdScreenH();

    int bottom = screenH;
    if (lcdKeyboardOnScreen() && s_kb.keys && lv_obj_is_valid(s_kb.keys)) {
        const int kh = lv_obj_get_height(s_kb.keys);
        if (kh > 0 && kh < screenH) bottom = screenH - kh;
    }

    int top = 0;
    if (o && lv_obj_is_valid(o)) {
        lv_obj_t* layer = topLevelOf(o);
        lv_area_t a;
        lv_obj_get_coords(layer, &a);
        top = a.y1 - lv_obj_get_style_translate_y(layer, LV_PART_MAIN);
        if (top < 0)                top = 0;
        if (top > LCD_STATUSBAR_H)  top = LCD_STATUSBAR_H;
    }

    const int room = bottom - top - 2 * lcdPx(4);
    return room > 0 ? room : 0;
}

void lcdKeyboardOpen(lv_obj_t* ta) { kbOpen(ta, ta, false); }

void lcdKeyboardOpenKeys(lv_obj_t* sink, lv_obj_t* anchor) { kbOpen(sink, anchor, true); }

void lcdKeyboardClose(void) { kbClose(); }

void lcdKeyboardAttach(lv_obj_t* ta) {
    if (!ta) return;
    lv_obj_add_event_cb(ta, kbTapCb, LV_EVENT_CLICKED, nullptr);
}
