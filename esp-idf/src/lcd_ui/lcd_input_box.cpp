/**
 * lcd_input_box.cpp — the generic text entry (see lcd_input_box.h) and the
 * caret-active state a pointing HAL reads (lcdCaretActive / lcdCaretRelease).
 *
 * The caret state is a single-box global: only one text field edits at a time, so
 * there's no need to track more. The board polls lcdCaretActive() every pointer
 * read; the widget flips it on a click/keystroke and the board flips it off (via
 * lcdCaretRelease) on a walk-out. Blink is toggled by the CURSOR part's opacity —
 * removing the local prop restores the theme's blinking cursor, forcing it
 * transparent freezes it hidden while keyboard focus stays on the box.
 *
 * Lcd task only.
 */
#include "lcd_input_box.h"
#include "lcd.h"
#include "lcd_internal.h"   /* lcdPointerHide */

#include <cstring>
#include <string>

namespace {

struct InputCtx {
    int      minLines = 1;
    int      maxLines = 4;
    bool     submitOnEnter = true;
    uint32_t lastSpaceTick = 0;   /* for the double-space → ". " window */
    bool     wantConvert   = false;
    bool     converting    = false;
    bool     capsNext      = false;   /* after ". ": capitalize the next letter */
    std::string trimBuf;          /* backing for lcdInputBoxText() */
};

/* The one box whose caret is live, and whether it currently is. */
lv_obj_t* s_caretBox = nullptr;
bool      s_caretOn  = false;

void caretShow(lv_obj_t* ta) {   /* restore the theme's blinking cursor */
    if (ta) lv_obj_remove_local_style_prop(ta, LV_STYLE_OPA, LV_PART_CURSOR);
}
void caretHide(lv_obj_t* ta) {   /* freeze it invisible without dropping focus */
    if (ta) lv_obj_set_style_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
}
void caretActivate(lv_obj_t* ta) {
    s_caretBox = ta;
    s_caretOn  = true;
    caretShow(ta);
    lcdPointerHide();   /* editing → trackball drives arrows; the ball leaves at once */
}

/* Size the box to its content, clamped to [minLines, maxLines]. We force a layout
 * FIRST so the label's wrapped height is current (measuring before that layout is
 * what lagged a line behind), set the exact height, then — when everything fits —
 * pin the internal scroll to the top. That last step kills the phantom scroll LVGL
 * leaves after briefly showing a shorter box: a scrollbar with the first line
 * scrolled off and the caret hidden below. Past maxLines the box keeps LVGL's
 * cursor-following scroll. */
void growNow(lv_obj_t* ta, InputCtx* c) {
    lv_obj_t* label = lv_textarea_get_label(ta);
    if (!label) return;
    const lv_font_t* f = lv_obj_get_style_text_font(ta, LV_PART_MAIN);
    int lineH = lv_font_get_line_height(f);
    if (lineH < 1) lineH = 1;
    lv_obj_update_layout(ta);                              /* re-wrap at the current width */
    int contentLines = (lv_obj_get_height(label) + lineH / 2) / lineH;
    if (contentLines < 1) contentLines = 1;
    int lines = contentLines;
    if (lines < c->minLines) lines = c->minLines;
    if (lines > c->maxLines) lines = c->maxLines;
    int padT = lv_obj_get_style_pad_top(ta, LV_PART_MAIN);
    int padB = lv_obj_get_style_pad_bottom(ta, LV_PART_MAIN);
    int bw   = lv_obj_get_style_border_width(ta, LV_PART_MAIN);
    lv_obj_set_height(ta, lines * lineH + padT + padB + 2 * bw);
    lv_obj_update_layout(ta);
    if (contentLines <= c->maxLines) lv_obj_scroll_to_y(ta, 0, LV_ANIM_OFF);
}

/* Fire LV_EVENT_READY out of band: we veto the Enter insert inside the INSERT
 * event, so there's no VALUE_CHANGED to ride — hand the submit to the next loop. */
void asyncReady(void* ta) {
    if (ta && lv_obj_is_valid((lv_obj_t*)ta))
        lv_obj_send_event((lv_obj_t*)ta, LV_EVENT_READY, nullptr);
}

/* Preprocess (runs BEFORE the textarea's own key handling): the first backspace
 * while the caps mode is armed is eaten — zero the key so the textarea deletes
 * nothing — and just cancel the mode. Must be preprocess; a normal callback runs
 * after the delete has already happened. */
void keyPreCb(lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_target_obj(e);
    InputCtx* c  = (InputCtx*)lv_obj_get_user_data(ta);
    if (!c || !c->capsNext) return;
    uint32_t* kp = (uint32_t*)lv_event_get_param(e);
    if (kp && *kp == LV_KEY_BACKSPACE) {
        *kp = 0;
        c->capsNext = false;
    }
}

void boxEvent(lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_target_obj(e);
    InputCtx* c  = (InputCtx*)lv_obj_get_user_data(ta);
    if (!c) return;
    switch (lv_event_get_code(e)) {

    case LV_EVENT_INSERT: {
        const char* ins = (const char*)lv_event_get_param(e);
        if (!ins || !ins[0]) break;
        if (c->capsNext) {                            /* first letter after ". " → capital */
            c->capsNext = false;
            if (!ins[1] && ins[0] >= 'a' && ins[0] <= 'z') {
                /* set_insert_replace stores the POINTER and reads it after this
                 * handler returns — must outlive the stack. Lcd task is single
                 * threaded and the insert completes before the next event. */
                static char up[2];
                up[0] = (char)(ins[0] - 'a' + 'A');
                up[1] = 0;
                lv_textarea_set_insert_replace(ta, up);
                break;
            }
            /* a non-letter just ends the mode — fall through to normal handling */
        }
        if (c->submitOnEnter && ins[0] == '\n' && ins[1] == 0) {
            lv_textarea_set_insert_replace(ta, "");   /* no newline — submit instead */
            lv_async_call(asyncReady, ta);
            break;
        }
        if (ins[0] == ' ' && ins[1] == 0) {           /* a lone space */
            uint32_t now = lv_tick_get();
            if (c->lastSpaceTick && (uint32_t)(now - c->lastSpaceTick) < 300) c->wantConvert = true;
            c->lastSpaceTick = now;
        } else {
            c->lastSpaceTick = 0;
        }
        break;
    }

    case LV_EVENT_VALUE_CHANGED: {
        if (c->converting) break;                     /* our own del/add below */
        caretActivate(ta);                            /* typing re-lights the caret */
        if (c->wantConvert) {                         /* "  " → ". ", then arm caps */
            c->wantConvert = false;
            c->converting  = true;
            lv_textarea_delete_char(ta);                 /* the 2nd space */
            lv_textarea_delete_char(ta);                 /* the 1st space */
            lv_textarea_add_text(ta, ". ");
            c->lastSpaceTick = 0;
            c->converting = false;
            c->capsNext = true;
        }
        /* An empty field starts a fresh sentence → arm caps so the first letter is
         * a capital (a leading backspace, eaten by keyPreCb, cancels it and fires
         * no VALUE_CHANGED, so it stays off). */
        { const char* t = lv_textarea_get_text(ta); if (!t || !t[0]) c->capsNext = true; }
        growNow(ta, c);
        break;
    }

    case LV_EVENT_FOCUSED:
    case LV_EVENT_PRESSED:
    case LV_EVENT_CLICKED:
        caretActivate(ta);                            /* a click grabs edit mode */
        break;

    case LV_EVENT_DELETE:
        if (s_caretBox == ta) { s_caretBox = nullptr; s_caretOn = false; }
        delete c;
        break;

    default:
        break;
    }
}

}  // namespace

lv_obj_t* lcdInputBoxCreate(lv_obj_t* parent, int minLines, int maxLines) {
    if (minLines < 1) minLines = 1;
    if (maxLines < minLines) maxLines = minLines;
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, false);              /* multi-line, so it can grow */
    InputCtx* c = new InputCtx();
    c->minLines = minLines;
    c->maxLines = maxLines;
    c->capsNext = true;   /* an empty field starts capitalized */
    lv_obj_set_user_data(ta, c);
    lv_obj_add_event_cb(ta, boxEvent, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(ta, keyPreCb, (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), nullptr);
    growNow(ta, c);                                   /* start at minLines */
    return ta;
}

void lcdInputBoxActivate(lv_obj_t* box) {
    if (!box) return;
    lv_group_t* g = lv_obj_get_group(box);
    if (g && lv_group_get_focused(g) != box) lv_group_focus_obj(box);   /* only if needed — avoids a scroll */
    caretActivate(box);
}

void lcdInputBoxSetLines(lv_obj_t* box, int minLines, int maxLines) {
    InputCtx* c = (InputCtx*)lv_obj_get_user_data(box);
    if (!c) return;
    if (minLines < 1) minLines = 1;
    if (maxLines < minLines) maxLines = minLines;
    c->minLines = minLines;
    c->maxLines = maxLines;
    growNow(box, c);
}

const char* lcdInputBoxText(lv_obj_t* box) {
    InputCtx* c = (InputCtx*)lv_obj_get_user_data(box);
    if (!c) return "";
    const char* t = lv_textarea_get_text(box);
    c->trimBuf = t ? t : "";
    while (!c->trimBuf.empty()) {
        char ch = c->trimBuf.back();
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') c->trimBuf.pop_back();
        else break;
    }
    return c->trimBuf.c_str();
}

void lcdInputBoxSetSubmitOnEnter(lv_obj_t* box, bool on) {
    InputCtx* c = (InputCtx*)lv_obj_get_user_data(box);
    if (c) c->submitOnEnter = on;
}

bool lcdCaretActive(int* x, int* y, bool* atTop) {
    if (!s_caretOn || !s_caretBox || !lv_obj_is_valid(s_caretBox)) return false;
    lv_obj_t* label = lv_textarea_get_label(s_caretBox);
    if (!label) return false;
    uint32_t cur = lv_textarea_get_cursor_pos(s_caretBox);
    lv_point_t p;
    lv_label_get_letter_pos(label, cur, &p);
    lv_area_t a;
    lv_obj_get_coords(label, &a);
    if (x)     *x = a.x1 + p.x;
    if (y)     *y = a.y1 + p.y;
    if (atTop) *atTop = (p.y <= 0);
    return true;
}

void lcdCaretRelease(void) {
    s_caretOn = false;
    if (s_caretBox && lv_obj_is_valid(s_caretBox)) caretHide(s_caretBox);
}
