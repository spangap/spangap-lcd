/**
 * lcd_settings_desc.cpp — the generic runtime behind the settings descriptors
 * (lcd_settings_desc.h): actions, dialogs, forms and collections.
 *
 * Everything here is DATA-DRIVEN. A straddle's `settings:` block lowers to
 * static structs and one call; this file is the single place that knows what
 * "confirm then write a key", "collect fields and submit them to a sentinel" or
 * "list an array with an editor" actually mean. That is why a straddle adopting
 * a collection deletes a pane rather than writing one.
 *
 * Two firmware conventions do the work that would otherwise be UI logic:
 *
 *   The firmware publishes FINISHED STRINGS. A title, a subtitle, a status pill,
 *   a value row — whatever the key holds is rendered verbatim. Nothing here
 *   formats, composes or compares; a `when_key` gate is a truthiness test.
 *
 *   The firmware VALIDATES IN SENTINEL HANDLERS. The UI never mutates a
 *   collection's array: it writes `<cmd>.add` / `.remove` / `.set` / `.order`
 *   and the owning task is the array's only writer. The handler answers on two
 *   keys shared by all of one collection's sentinels: a rejection is text on
 *   `<cmd>.error` (the form shows it and stays open), an accepted submission
 *   bumps `<cmd>.done` (the form closes). A bare form uses `<form-cmd>.error` /
 *   `.done` the same way. There is no validation on this side, and no
 *   per-keystroke checking anywhere.
 *
 * Everything is event-driven — callbacks and storage subscriptions, no waits and
 * no polling. storageSet is asynchronous (it queues to the owning actor), so
 * nothing here reads a key back after writing it; it relies on operation order.
 *
 * Lcd task only.
 */
#include "lcd.h"
#include "lcd_internal.h"
#include "lcd_settings_priv.h"

#include "storage.h"
#include "log.h"

#include <cJSON.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace {

/* ---- template substitution ----
 * `{field}` replacement and nothing else: no expressions, no fallbacks, no
 * slicing. Anything fancier is a string the firmware publishes ready-made. */

/** What a template resolves against inside a collection: one item's storage
 *  prefix, and what `{id}` means there. Empty outside a collection. */
struct ItemScope {
    std::string prefix;     /* e.g. "s.tcp.peers.2" */
    std::string idField;
    std::string idValue;
};

std::string fieldOf(const ItemScope& sc, const std::string& name) {
    if (sc.prefix.empty()) return "";
    if (name == "id" && !sc.idValue.empty()) return sc.idValue;
    return storageGetStr((sc.prefix + "." + name).c_str(), "");
}

/** Generic `{field}` walk; `lookup` answers one field. */
template <typename F>
std::string substWith(const char* tmpl, F lookup) {
    std::string out;
    if (!tmpl) return out;
    for (const char* p = tmpl; *p; ) {
        if (*p != '{') { out += *p++; continue; }
        const char* close = strchr(p, '}');
        if (!close) { out += *p++; continue; }
        out += lookup(std::string(p + 1, close - p - 1));
        p = close + 1;
    }
    return out;
}

std::string subst(const char* tmpl, const ItemScope& sc) {
    return substWith(tmpl, [&](const std::string& n) { return fieldOf(sc, n); });
}

/* A gate key is TRUTHY or it is not. Never an equality test — the firmware
 * publishes gate keys as truthy/empty exactly so this stays a one-liner. */
bool truthy(const std::string& v) { return !v.empty() && v != "0"; }

/* ---- shared modal scaffolding ---- */

/* Every open modal overlay, so lcdSettingsDescReset() can take them down when
 * the Settings app itself goes away — they live on lv_layer_top, outside the
 * app's widget tree, so nothing else would. */
std::vector<lv_obj_t*> s_modals;

void modalUntrack(lv_obj_t* ov) {
    for (auto it = s_modals.begin(); it != s_modals.end(); ++it)
        if (*it == ov) { s_modals.erase(it); return; }
}

void modalDelete(lv_event_t* e) {
    modalUntrack(static_cast<lv_obj_t*>(lv_event_get_target(e)));
}

/** Full-screen dim overlay carrying a centred card. Returns the card (build the
 *  content into it); `*overlayOut` is what to delete to dismiss. */
lv_obj_t* makeModal(lv_obj_t** overlayOut, const char* title) {
    lv_obj_t* ov = lv_obj_create(lv_layer_top());
    s_modals.push_back(ov);
    lv_obj_add_event_cb(ov, modalDelete, LV_EVENT_DELETE, nullptr);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_70, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);    /* swallow taps behind the card */

    lv_obj_t* card = lv_obj_create(ov);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(88));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, lv_pct(88), 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1b2129), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x3a4658), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    if (title && *title) {
        lv_obj_t* t = lv_label_create(card);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, lv_color_white(), 0);
        lv_obj_set_style_text_font(t, lcdFont(LcdFace::UI_BOLD, lcdPx(16)), 0);
    }
    *overlayOut = ov;
    return card;
}

/** A full-width button, used for both modal buttons and the pane-level ones a
 *  collection adds (Add / Scan). Same shape as lcdSettingButton's, but taking
 *  an lv_event_cb_t so it can carry a context pointer. */
lv_obj_t* wideButton(lv_obj_t* parent, const char* label, bool danger,
                     lv_event_cb_t cb, void* ud) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_width(b, lv_pct(100));
    if (danger) lv_obj_set_style_bg_color(b, lv_color_hex(0x8b2b2b), 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return b;
}

/* Dismissing a modal from one of its own buttons deletes an ancestor of the
 * widget whose event is running — LVGL wants that deferred. Untracked here, at
 * dismiss time, so a reset that runs before the deferred delete fires cannot
 * delete the object a second time. */
void dismiss(lv_obj_t* overlay) {
    if (!overlay) return;
    modalUntrack(overlay);
    lv_obj_delete_async(overlay);
}

/* ---- refcounted storage subscriptions ----
 * storageUnsubscribe() drops EVERY subscription this task holds on a scope, so
 * neither of two users of one scope may unsubscribe while the other lives.
 * Refcount PER SCOPE — not per (scope, callback): with per-pair counts, a form
 * closing its watch on a collection's array would unsubscribe the scope out
 * from under the collection's own live count. One dispatcher (descDispatch)
 * serves every subscriber in this file, so a scope needs exactly one real
 * subscription however many users it has.
 *
 * The plain-row binding table (lcd_settings.cpp) unsubscribes its exact keys
 * through the same storage API on the same task; its scopes are leaf keys and
 * ours are array/status prefixes and sentinel-answer keys, which never collide
 * as exact strings — the invariant both tables rely on. */

void descDispatch(const char* key, const char* val);

struct SubRef { std::string scope; int refs; };
std::vector<SubRef> s_subs;

void subAdd(const std::string& scope) {
    for (auto& s : s_subs)
        if (s.scope == scope) { s.refs++; return; }
    s_subs.push_back({ scope, 1 });
    storageSubscribeChanges(scope.c_str(), descDispatch);
}

void subDrop(const std::string& scope) {
    for (auto it = s_subs.begin(); it != s_subs.end(); ++it) {
        if (it->scope != scope) continue;
        if (--it->refs > 0) return;
        s_subs.erase(it);
        storageUnsubscribe(scope.c_str());
        return;
    }
}

/** The literal prefix of a template — everything before the first `{`. Storage
 *  subscriptions are prefix-matched, so one subscription on this covers every
 *  key the template can produce. */
std::string literalPrefix(const char* tmpl) {
    if (!tmpl) return "";
    const char* brace = strchr(tmpl, '{');
    return brace ? std::string(tmpl, brace - tmpl) : std::string(tmpl);
}

/* ---- status pills ----
 * The key holds packed "text|color": both halves finished by the firmware, so
 * the pill neither maps states to words nor decides what a state's colour is. */

struct Pill { std::string key; lv_obj_t* lbl; };
std::vector<Pill> s_pills;

lv_color_t pillColor(const std::string& name) {
    if (name.empty())        return lv_color_hex(0x3a4658);
    if (name == "green")     return lv_color_hex(0x2e7d43);
    if (name == "red")       return lv_color_hex(0x8b2b2b);
    if (name == "amber")     return lv_color_hex(0x8a6d1f);
    if (name == "blue")      return lv_color_hex(0x2563a0);
    if (name == "grey" || name == "gray") return lv_color_hex(0x3a4658);
    const char* s = name.c_str();
    if (*s == '#') s++;
    return lv_color_hex((uint32_t)strtoul(s, nullptr, 16));   /* an explicit rrggbb */
}

void pillApply(lv_obj_t* lbl, const char* packed) {
    std::string v = packed ? packed : "";
    size_t bar = v.find('|');
    std::string text  = bar == std::string::npos ? v : v.substr(0, bar);
    std::string color = bar == std::string::npos ? "" : v.substr(bar + 1);
    if (text.empty()) { lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lbl, text.c_str());
    lv_obj_set_style_bg_color(lbl, pillColor(color), 0);
}

void pillDelete(lv_event_t* e);

lv_obj_t* makePill(lv_obj_t* parent, const std::string& key) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 6, 0);
    lv_obj_set_style_pad_hor(l, 6, 0);
    lv_obj_set_style_pad_ver(l, 1, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, lcdFont(LcdFace::UI, lcdPx(12)), 0);
    s_pills.push_back({ key, l });
    lv_obj_add_event_cb(l, pillDelete, LV_EVENT_DELETE, nullptr);
    pillApply(l, storageGetStr(key.c_str(), "").c_str());
    return l;
}

void pillDelete(lv_event_t* e) {
    lv_obj_t* w = static_cast<lv_obj_t*>(lv_event_get_target(e));
    for (auto it = s_pills.begin(); it != s_pills.end(); ++it)
        if (it->lbl == w) { s_pills.erase(it); return; }
}

/* ---- forms ----
 * One form is open at a time; the screen has room for exactly one. Values live
 * in a local buffer and reach the device only on submit — that is what makes
 * submit-and-error possible in place of per-keystroke validation. */

struct FormField {
    const lcd_row_t* row;
    lv_obj_t* rowObj  = nullptr;    /* the container, hidden/shown by when_key */
    lv_obj_t* widget  = nullptr;
    lv_obj_t* valLbl  = nullptr;    /* text rows on the on-screen-keyboard path */
    std::string value;
    bool dirty = false;             /* operator typed here: a template default freezes */
};

struct FormCtx {
    const lcd_form_t* d = nullptr;
    lv_obj_t* overlay   = nullptr;
    lv_obj_t* errLbl    = nullptr;
    std::vector<FormField> fields;
    std::string cmdKey, errKey, ackKey;
    std::string editId;             /* the id an item editor is committing against */
    ItemScope   sc;
    const void* owner = nullptr;    /* the CollCtx this form belongs to, or null */
    bool submitted = false;
};
FormCtx* s_form = nullptr;

std::string formLookup(const std::string& name) {
    if (!s_form) return "";
    for (auto& f : s_form->fields) {
        const char* fn = f.row->field;
        if (fn && name == fn) return f.value;
    }
    return "";
}

void formRefresh();          /* fwd: re-evaluate gates + untouched defaults */
void formClose();            /* from the form's own buttons: deferred delete */
void formForceClose();       /* from outside its events: synchronous delete */

/** The n'th entry of a newline-separated list, or "" past the end. */
std::string nthOption(const char* list, uint32_t n) {
    std::string all(list ? list : "");
    size_t start = 0;
    for (uint32_t i = 0; ; i++) {
        size_t nl = all.find('\n', start);
        std::string one = all.substr(start, nl == std::string::npos ? nl : nl - start);
        if (i == n) return one;
        if (nl == std::string::npos) return "";
        start = nl + 1;
    }
}

/** Select the option whose VALUE equals `value`. A form dropdown lists the
 *  labels (the list is for a person) and stores the values, so it can only be
 *  driven by index — matching on the visible text would compare a label against
 *  a value and silently select nothing. */
void dropdownSelectValue(lv_obj_t* d, const lcd_row_t* r, const std::string& value) {
    std::string all(r->options ? r->options : "");
    size_t start = 0;
    for (uint32_t i = 0; ; i++) {
        size_t nl = all.find('\n', start);
        std::string one = all.substr(start, nl == std::string::npos ? nl : nl - start);
        if (one == value) { lv_dropdown_set_selected(d, i); return; }
        if (nl == std::string::npos) return;
        start = nl + 1;
    }
}

void formApplyValue(FormField& f) {
    switch (f.row->kind) {
        case LCD_ROW_SWITCH:
            if (truthy(f.value)) lv_obj_add_state(f.widget, LV_STATE_CHECKED);
            else                 lv_obj_remove_state(f.widget, LV_STATE_CHECKED);
            break;
        case LCD_ROW_SLIDER:
            lv_slider_set_value(f.widget, atoi(f.value.c_str()), LV_ANIM_OFF);
            break;
        case LCD_ROW_DROPDOWN:
            dropdownSelectValue(f.widget, f.row, f.value);
            break;
        case LCD_ROW_TEXT:
            if (f.valLbl) lcdSettingsValueText(f.valLbl, f.value.c_str(), f.row->secret);
            else if (f.widget && !(lv_obj_get_state(f.widget) & LV_STATE_FOCUSED))
                lv_textarea_set_text(f.widget, f.value.c_str());
            break;
        default: break;
    }
}

void formRefresh() {
    if (!s_form) return;
    for (auto& f : s_form->fields) {
        /* An untouched template default tracks its siblings; once the operator
         * has typed in the field it is theirs and stops moving under them. */
        if (!f.dirty && f.row->dflt && *f.row->dflt) {
            std::string v = substWith(f.row->dflt, formLookup);
            if (v != f.value) { f.value = v; formApplyValue(f); }
        }
        if (f.row->when_key && *f.row->when_key && f.rowObj) {
            /* A gate written as a template ("{dhcp}") names a SIBLING FIELD and
             * is answered from the local buffer; a bare key names storage. */
            bool on = strchr(f.row->when_key, '{')
                ? truthy(substWith(f.row->when_key, formLookup))
                : truthy(storageGetStr(f.row->when_key, ""));
            if (on) lv_obj_remove_flag(f.rowObj, LV_OBJ_FLAG_HIDDEN);
            else    lv_obj_add_flag   (f.rowObj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* -- the on-screen-keyboard editor for one form field -- */
struct KbCtx { FormField* f; lv_obj_t* overlay; lv_obj_t* ta; };
KbCtx s_kb;

void kbClose(bool commit) {
    if (commit && s_kb.f) {
        s_kb.f->value = lv_textarea_get_text(s_kb.ta);
        s_kb.f->dirty = true;
        formApplyValue(*s_kb.f);
        formRefresh();
    }
    if (s_kb.overlay) { lv_obj_delete(s_kb.overlay); s_kb.overlay = nullptr; }
    s_kb.f = nullptr;
}

void kbEvent(lv_event_t* e) {
    lv_event_code_t c = lv_event_get_code(e);
    if      (c == LV_EVENT_READY)  kbClose(true);
    else if (c == LV_EVENT_CANCEL) kbClose(false);
}

void onFieldTap(lv_event_t* e) {
    auto* f = static_cast<FormField*>(lv_event_get_user_data(e));
    s_kb.f = f;

    lv_obj_t* ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    s_kb.overlay = ov;

    lv_obj_t* ta = lv_textarea_create(ov);
    lv_obj_set_size(ta, lv_pct(96), 56);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 6);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, f->row->secret);
    lv_textarea_set_text(ta, f->value.c_str());
    s_kb.ta = ta;

    lv_obj_t* kb = lv_keyboard_create(ov);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, kbEvent, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(kb, kbEvent, LV_EVENT_CANCEL, nullptr);
}

void onFieldInline(lv_event_t* e) {          /* physical keyboard: edit in place */
    auto* f = static_cast<FormField*>(lv_event_get_user_data(e));
    f->value = lv_textarea_get_text(static_cast<lv_obj_t*>(lv_event_get_target(e)));
    f->dirty = true;
    formRefresh();
}

void onFieldSwitch(lv_event_t* e) {
    auto* f = static_cast<FormField*>(lv_event_get_user_data(e));
    f->value = lv_obj_has_state(static_cast<lv_obj_t*>(lv_event_get_target_obj(e)),
                                LV_STATE_CHECKED) ? "1" : "0";
    f->dirty = true;
    formRefresh();
}

void onFieldSlider(lv_event_t* e) {
    auto* f = static_cast<FormField*>(lv_event_get_user_data(e));
    char buf[16];
    snprintf(buf, sizeof(buf), "%d",
             (int)lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target_obj(e))));
    f->value = buf;
    f->dirty = true;
    formRefresh();
}

void onFieldDropdown(lv_event_t* e) {
    auto* f = static_cast<FormField*>(lv_event_get_user_data(e));
    lv_obj_t* d = static_cast<lv_obj_t*>(lv_event_get_target_obj(e));
    /* The list shows labels and stores values, so index across, not text. */
    f->value = nthOption(f->row->options, lv_dropdown_get_selected(d));
    f->dirty = true;
    formRefresh();
}

void buildFormField(lv_obj_t* parent, FormField& f) {
    const lcd_row_t* r = f.row;
    if (r->kind == LCD_ROW_SECTION) { f.rowObj = lcdSettingSection(parent, r->label); return; }
    if (r->kind == LCD_ROW_CAPTION) { f.rowObj = lcdSettingCaption(parent, r->label); return; }

    lv_obj_t* row = lcdSettingsMakeRow(parent);
    lcdSettingsRowLabel(row, r->label);
    f.rowObj = row;

    switch (r->kind) {
        case LCD_ROW_SWITCH: {
            lv_obj_t* sw = lv_switch_create(row);
            lv_obj_set_size(sw, 36, 18);
            f.widget = sw;
            lv_obj_add_event_cb(sw, onFieldSwitch, LV_EVENT_VALUE_CHANGED, &f);
            break;
        }
        case LCD_ROW_SLIDER: {
            int lo = r->min, hi = r->max;
            if (r->min_key) lo = storageGetInt(r->min_key, lo);
            if (r->max_key) hi = storageGetInt(r->max_key, hi);
            if (hi < lo) hi = lo;
            lv_obj_t* s = lv_slider_create(row);
            lcdSettingsFillControl(s);
            lv_slider_set_range(s, lo, hi);
            f.widget = s;
            lv_obj_add_event_cb(s, onFieldSlider, LV_EVENT_VALUE_CHANGED, &f);
            break;
        }
        case LCD_ROW_DROPDOWN: {
            lv_obj_t* d = lv_dropdown_create(row);
            lcdSettingsFillControl(d);
            /* Show the labels, store the values — the list is for a person. */
            lv_dropdown_set_options(d, (r->opt_labels && *r->opt_labels) ? r->opt_labels
                                                                        : r->options);
            f.widget = d;
            lv_obj_add_event_cb(d, onFieldDropdown, LV_EVENT_VALUE_CHANGED, &f);
            break;
        }
        case LCD_ROW_TEXT:
            if (lcdHasKeyboard()) {
                lv_obj_t* ta = lv_textarea_create(row);
                lv_textarea_set_one_line(ta, true);
                lv_textarea_set_password_mode(ta, r->secret);
                if (r->placeholder) lv_textarea_set_placeholder_text(ta, r->placeholder);
                lcdSettingsFillControl(ta);
                if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), ta);
                f.widget = ta;
                lv_obj_add_event_cb(ta, onFieldInline, LV_EVENT_READY,     &f);
                lv_obj_add_event_cb(ta, onFieldInline, LV_EVENT_DEFOCUSED, &f);
            } else {
                lv_obj_t* val = lv_label_create(row);
                lv_obj_set_style_text_color(val, lv_color_hex(0xb0b8c0), 0);
                lcdSettingsFillControl(val);
                lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_LEFT, 0);
                f.valLbl = val;
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(row, onFieldTap, LV_EVENT_CLICKED, &f);
            }
            break;
        default: {
            lv_obj_t* val = lv_label_create(row);
            lv_obj_set_style_text_color(val, lv_color_hex(0xb0b8c0), 0);
            lcdSettingsFillControl(val);
            lv_label_set_text(val, r->key ? storageGetStr(r->key, "").c_str() : "");
            break;
        }
    }
    formApplyValue(f);
}

void onFormSubmit(lv_event_t*) {
    if (!s_form) return;
    cJSON* obj = cJSON_CreateObject();
    for (auto& f : s_form->fields) {
        if (!f.row->field || !*f.row->field) continue;
        cJSON_AddStringToObject(obj, f.row->field, f.value.c_str());
    }
    /* An item editor carries the identity it is committing AGAINST, separately
     * from the fields — so editing the id field itself is an ordinary edit and
     * the owning task still knows which item to apply it to. */
    if (!s_form->editId.empty())
        cJSON_AddStringToObject(obj, "_id", s_form->editId.c_str());
    char* json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return;
    s_form->submitted = true;
    if (s_form->errLbl) lv_obj_add_flag(s_form->errLbl, LV_OBJ_FLAG_HIDDEN);
    /* Clear the device-side reason first, in order, so a rejection identical
     * to the last one is still a CHANGE the dispatcher can see — the storage
     * actor dedups same-value writes. */
    storageSet(s_form->errKey.c_str(), "");
    storageSet(s_form->cmdKey.c_str(), json);
    cJSON_free(json);
    /* No read-back and no timer: the owning task rejects (the reason lands on
     * the error key and we stay open showing it) or accepts (it bumps the ack
     * key and we close). An edit that changes nothing still acks. */
}

void onFormCancel(lv_event_t*) { formClose(); }

/** Detach the form's bookkeeping — subscriptions and the s_form pointer — and
 *  hand back the context; the caller decides how its overlay dies. The context
 *  itself is freed by the overlay's delete callback (field widgets' event
 *  callbacks point into it). */
FormCtx* formDetach() {
    FormCtx* f = s_form;
    if (!f) return nullptr;
    s_form = nullptr;                       /* before the deletes: no re-entry */
    subDrop(f->errKey);
    subDrop(f->ackKey);
    return f;
}

void formClose() {
    FormCtx* f = formDetach();
    if (!f) return;
    if (f->overlay) dismiss(f->overlay);    /* deferred: our own button event */
    else            delete f;
}

void formForceClose() {
    FormCtx* f = formDetach();
    if (!f) return;
    if (f->overlay) { modalUntrack(f->overlay); lv_obj_delete(f->overlay); }
    else            delete f;
}

void formCtxFree(lv_event_t* e) { delete static_cast<FormCtx*>(lv_event_get_user_data(e)); }

void formApply(const char* key, const char* val) {
    if (!s_form) return;
    if (s_form->errKey == key) {
        if (val && *val) {                  /* rejected: say why, stay open */
            if (s_form->errLbl) {
                lv_label_set_text(s_form->errLbl, val);
                lv_obj_remove_flag(s_form->errLbl, LV_OBJ_FLAG_HIDDEN);
            }
            s_form->submitted = false;
        }
        return;
    }
    /* The ack key moved. After a submit that is the acceptance. */
    if (s_form->ackKey == key && s_form->submitted) formClose();
}

/** Open a form. `errKey`/`ackKey` are where the owning task answers (a
 *  collection passes its shared `<cmd>.error` / `<cmd>.done`); `prefill` seeds
 *  fields by name; `editId` is set when this is an item editor rather than an
 *  add; `owner` ties the form to the collection whose teardown closes it. */
void formOpen(const lcd_form_t* d, const ItemScope& sc,
              const std::string& errKey, const std::string& ackKey,
              const std::vector<std::pair<std::string, std::string>>& prefill,
              const std::string& editId, const void* owner = nullptr) {
    if (s_form) formForceClose();
    FormCtx* ctx = new FormCtx();
    ctx->d        = d;
    ctx->sc       = sc;
    ctx->cmdKey   = subst(d->cmd, sc);
    ctx->errKey   = errKey;
    ctx->ackKey   = ackKey;
    ctx->editId   = editId;
    ctx->owner    = owner;
    ctx->fields.resize(d->nfields);
    for (int i = 0; i < d->nfields; i++) ctx->fields[i].row = &d->fields[i];
    s_form = ctx;

    /* Seed: an explicit prefill wins, then the item being edited, then the
     * field's own `default:` (a template until the operator types). */
    for (auto& f : ctx->fields) {
        const char* fn = f.row->field;
        if (!fn || !*fn) continue;
        bool seeded = false;
        for (auto& kv : prefill)
            if (kv.first == fn) { f.value = kv.second; seeded = true; break; }
        if (!seeded && !sc.prefix.empty() && !editId.empty()) {
            f.value = fieldOf(sc, fn);
            seeded  = !f.value.empty();
        }
        if (seeded) f.dirty = true;
    }

    lv_obj_t* card = makeModal(&ctx->overlay,
                               (d->title && *d->title) ? d->title : nullptr);
    lv_obj_add_event_cb(ctx->overlay, formCtxFree, LV_EVENT_DELETE, ctx);
    lv_obj_t* body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(body, lcdScreenH() / 2, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 4, 0);

    for (auto& f : ctx->fields) buildFormField(body, f);

    ctx->errLbl = lv_label_create(card);
    lv_label_set_long_mode(ctx->errLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->errLbl, lv_pct(100));
    lv_obj_set_style_text_color(ctx->errLbl, lv_color_hex(0xe06c6c), 0);
    /* Hidden until a verdict arrives; submit clears the device-side reason, so
     * a stale one from an earlier attempt is never shown. */
    lv_obj_add_flag(ctx->errLbl, LV_OBJ_FLAG_HIDDEN);

    wideButton(card, (d->submit && *d->submit) ? d->submit : "Save", false,
                onFormSubmit, nullptr);
    wideButton(card, "Cancel", false, onFormCancel, nullptr);

    subAdd(ctx->errKey);
    subAdd(ctx->ackKey);
    formRefresh();
}

/* ---- dialogs ---- */

struct DlgBtnCtx { lv_obj_t* overlay; const lcd_action_t* act; ItemScope sc; };

void dlgBtnFree(lv_event_t* e) { delete static_cast<DlgBtnCtx*>(lv_event_get_user_data(e)); }

void onDlgButton(lv_event_t* e) {
    auto* c = static_cast<DlgBtnCtx*>(lv_event_get_user_data(e));
    /* Copy out first: deleting the overlay frees this context. Every button
     * closes the dialog — a bare label is simply a cancel. */
    lv_obj_t* ov = c->overlay;
    const lcd_action_t* act = c->act;
    ItemScope sc = c->sc;
    lv_obj_delete(ov);
    if (act) lcdSettingRunAction(act, sc.prefix.empty() ? nullptr : sc.prefix.c_str(),
                                 sc.idValue.empty() ? nullptr : sc.idValue.c_str());
}

void dialogOpen(const lcd_dialog_t* d, const ItemScope& sc) {
    lv_obj_t* ov = nullptr;
    lv_obj_t* card = makeModal(&ov, nullptr);

    lv_obj_t* t = lv_label_create(card);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text(t, subst(d->text, sc).c_str());

    for (int i = 0; i < d->nbuttons; i++) {
        auto* c = new DlgBtnCtx{ ov, d->buttons[i].act, sc };
        lv_obj_t* b = wideButton(card, d->buttons[i].label, d->buttons[i].danger,
                                  onDlgButton, c);
        lv_obj_add_event_cb(b, dlgBtnFree, LV_EVENT_DELETE, c);
    }
}

/** The device is going away on purpose. Cover the screen so the UI can't be
 *  driven into a store that is about to be replaced; there is nothing to wait
 *  for and nothing to poll — the reboot takes the screen with it. */
void rebootNotice() {
    lv_obj_t* ov = nullptr;
    lv_obj_t* card = makeModal(&ov, nullptr);
    lv_obj_t* t = lv_label_create(card);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text(t, "Restarting the device.");
}

/* ---- collections ---- */

struct CollCtx {
    const lcd_collection_t* d;
    lv_obj_t* box     = nullptr;    /* item rows */
    lv_obj_t* candBox = nullptr;    /* candidate rows */
    std::string arrayScope, candScope, statusScope;
    /* Where every one of this collection's sentinels answers. */
    std::string errKey, ackKey;
    /* The item editor, assembled once from `edit:` + `<cmd>.set`. It has to
     * outlive the click that opens it, and there is one per collection. */
    lcd_form_t  editForm{};
    std::string setCmd;
};
std::vector<CollCtx*> s_colls;

/** Confirm-then-write, as a plain dialog: the one live button writes the
 *  sentinel, everything else just closes. */
void confirmRemove(const std::string& text, const std::string& key, const std::string& id) {
    struct RmCtx { lv_obj_t* ov; std::string key, id; };
    lv_obj_t* ov = nullptr;
    lv_obj_t* card = makeModal(&ov, nullptr);
    lv_obj_t* t = lv_label_create(card);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text(t, text.c_str());

    auto* rc = new RmCtx{ ov, key, id };
    lv_obj_add_event_cb(ov, [](lv_event_t* e) {
        delete static_cast<RmCtx*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, rc);
    wideButton(card, "Remove", true, [](lv_event_t* e) {
        auto* r = static_cast<RmCtx*>(lv_event_get_user_data(e));
        std::string k = r->key, id = r->id;      /* the dismiss frees r */
        dismiss(r->ov);
        storageSet(k.c_str(), id.c_str());
    }, rc);
    wideButton(card, "Cancel", false, [](lv_event_t* e) {
        dismiss(static_cast<RmCtx*>(lv_event_get_user_data(e))->ov);
    }, rc);
}

void collRebuild(CollCtx* c);
void candRebuild(CollCtx* c);

/** One item's storage prefix and identity. */
ItemScope itemScope(const lcd_collection_t* d, int idx) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s.%d", d->key, idx);
    ItemScope sc;
    sc.prefix  = buf;
    sc.idField = d->id;
    sc.idValue = storageGetStr((sc.prefix + "." + d->id).c_str(), "");
    return sc;
}

/** The current id order of the array, comma-joined — what a reorder writes. */
std::string idOrder(const lcd_collection_t* d, int moveFrom, int moveTo) {
    int n = storageArrayCount(d->key);
    std::vector<std::string> ids;
    for (int i = 0; i < n; i++) ids.push_back(itemScope(d, i).idValue);
    if (moveFrom >= 0 && moveFrom < n && moveTo >= 0 && moveTo < n)
        std::swap(ids[moveFrom], ids[moveTo]);
    std::string out;
    for (size_t i = 0; i < ids.size(); i++) {
        if (i) out += ",";
        out += ids[i];
    }
    return out;
}

/* Per-row button contexts. Heap-allocated per rebuild, freed with their row. */
struct ItemBtnCtx {
    CollCtx* c;
    int idx;
    const lcd_action_t* act;    /* per-item action, or null */
    int move;                   /* reorder delta, 0 when not a reorder button */
    bool remove;
    bool edit;
};
void itemBtnFree(lv_event_t* e) { delete static_cast<ItemBtnCtx*>(lv_event_get_user_data(e)); }

void onItemButton(lv_event_t* e) {
    auto* b = static_cast<ItemBtnCtx*>(lv_event_get_user_data(e));
    const lcd_collection_t* d = b->c->d;
    ItemScope sc = itemScope(d, b->idx);

    if (b->move) {
        storageSet((std::string(d->cmd) + ".order").c_str(),
                   idOrder(d, b->idx, b->idx + b->move).c_str());
        return;
    }
    if (b->remove) {
        std::string key = std::string(d->cmd) + ".remove";
        if (!(d->remove_confirm && *d->remove_confirm)) {
            storageSet(key.c_str(), sc.idValue.c_str());
            return;
        }
        confirmRemove(subst(d->remove_confirm, sc), key, sc.idValue);
        return;
    }
    if (b->edit) {
        /* The item editor is the collection's `edit:` rows over `<cmd>.set` —
         * the sentinel family stays derived from the one `cmd` name, so the
         * descriptor never spells the keys out. */
        formOpen(&b->c->editForm, sc, b->c->errKey, b->c->ackKey, {}, sc.idValue, b->c);
        return;
    }
    if (b->act) lcdSettingRunAction(b->act, sc.prefix.c_str(), sc.idValue.c_str());
}

lv_obj_t* itemRowButton(lv_obj_t* parent, const char* txt, ItemBtnCtx* ctx, bool danger) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_style_pad_hor(b, 8, 0);
    lv_obj_set_style_pad_ver(b, 3, 0);
    if (danger) lv_obj_set_style_bg_color(b, lv_color_hex(0x8b2b2b), 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, onItemButton, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(b, itemBtnFree, LV_EVENT_DELETE, ctx);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return b;
}

lv_obj_t* listRow(lv_obj_t* parent) {
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(r, 8, 0);
    lv_obj_set_style_pad_ver(r, 3, 0);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/** Title over an optional subtitle, both rendered exactly as published. */
lv_obj_t* titleBlock(lv_obj_t* row, const std::string& title, const std::string& sub) {
    lv_obj_t* col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* t = lv_label_create(col);
    lv_label_set_text(t, title.c_str());
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_color(t, lv_color_white(), 0);

    if (!sub.empty()) {
        lv_obj_t* s = lv_label_create(col);
        lv_label_set_text(s, sub.c_str());
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s, lv_pct(100));
        lv_obj_set_style_text_color(s, lv_color_hex(0x8a93a0), 0);
        lv_obj_set_style_text_font(s, lcdFont(LcdFace::MONO, lcdPx(12)), 0);
    }
    return col;
}

void collRebuild(CollCtx* c) {
    if (!c->box) return;
    lv_obj_clean(c->box);
    const lcd_collection_t* d = c->d;
    int n = storageArrayCount(d->key);
    if (n <= 0) {
        if (d->empty && *d->empty) lcdSettingCaption(c->box, d->empty);
        return;
    }
    for (int i = 0; i < n; i++) {
        ItemScope sc = itemScope(d, i);
        lv_obj_t* row = listRow(c->box);
        titleBlock(row, subst(d->item, sc),
                   (d->subtitle && *d->subtitle) ? subst(d->subtitle, sc) : "");

        if (d->status && *d->status) makePill(row, subst(d->status, sc));

        if (d->orderable) {
            if (i > 0)     itemRowButton(row, LV_SYMBOL_UP,
                                         new ItemBtnCtx{ c, i, nullptr, -1, false, false }, false);
            if (i < n - 1) itemRowButton(row, LV_SYMBOL_DOWN,
                                         new ItemBtnCtx{ c, i, nullptr, +1, false, false }, false);
        }
        for (int a = 0; a < d->nactions; a++)
            itemRowButton(row, d->actions[a].label,
                          new ItemBtnCtx{ c, i, d->actions[a].act, 0, false, false },
                          d->actions[a].danger);
        if (d->nedit > 0)
            itemRowButton(row, LV_SYMBOL_EDIT,
                          new ItemBtnCtx{ c, i, nullptr, 0, false, true }, false);
        if (d->has_remove)
            itemRowButton(row, LV_SYMBOL_TRASH,
                          new ItemBtnCtx{ c, i, nullptr, 0, true, false }, false);
    }
}

/* -- candidates -- */

struct CandCtx { CollCtx* c; int idx; };
void candCtxFree(lv_event_t* e) { delete static_cast<CandCtx*>(lv_event_get_user_data(e)); }

void onCandidatePick(lv_event_t* e) {
    auto* cc = static_cast<CandCtx*>(lv_event_get_user_data(e));
    const lcd_collection_t* d = cc->c->d;
    const lcd_candidates_t* cand = d->candidates;
    if (!cand || d->nadds == 0) return;

    char buf[96];
    snprintf(buf, sizeof(buf), "%s.%d", cand->key, cc->idx);
    ItemScope candSc;
    candSc.prefix = buf;

    /* Same-name fields carry over implicitly; `map:` covers only the renames. */
    std::vector<std::pair<std::string, std::string>> prefill;
    const lcd_form_t* form = d->adds[0].form;
    for (int i = 0; i < form->nfields; i++) {
        const char* fn = form->fields[i].field;
        if (!fn || !*fn) continue;
        std::string from = fn;
        std::string mapping = cand->map ? cand->map : "";
        size_t pos = 0;
        while (pos < mapping.size()) {                 /* "from:to,from:to" */
            size_t comma = mapping.find(',', pos);
            std::string pair = mapping.substr(pos, comma == std::string::npos
                                                   ? std::string::npos : comma - pos);
            size_t colon = pair.find(':');
            if (colon != std::string::npos && pair.substr(colon + 1) == fn)
                from = pair.substr(0, colon);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        std::string v = storageGetStr((candSc.prefix + "." + from).c_str(), "");
        if (!v.empty()) prefill.push_back({ fn, v });
    }
    formOpen(form, ItemScope{}, cc->c->errKey, cc->c->ackKey, prefill, "", cc->c);
}

void candRebuild(CollCtx* c) {
    if (!c->candBox) return;
    lv_obj_clean(c->candBox);
    const lcd_candidates_t* cand = c->d->candidates;
    int n = storageArrayCount(cand->key);
    for (int i = 0; i < n; i++) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s.%d", cand->key, i);
        ItemScope sc;
        sc.prefix = buf;
        lv_obj_t* row = listRow(c->candBox);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        titleBlock(row, subst(cand->item, sc),
                   (cand->subtitle && *cand->subtitle) ? subst(cand->subtitle, sc) : "");
        lv_obj_t* ch = lv_label_create(row);
        lv_label_set_text(ch, LV_SYMBOL_PLUS);
        lv_obj_set_style_text_color(ch, lv_color_hex(0x8a93a0), 0);
        auto* cc = new CandCtx{ c, i };
        lv_obj_add_event_cb(row, onCandidatePick, LV_EVENT_CLICKED, cc);
        lv_obj_add_event_cb(row, candCtxFree, LV_EVENT_DELETE, cc);
    }
}

/** THE storage callback for this whole file — pills, collection rebuilds and
 *  the open form all answer to it, which is what lets the subscription table
 *  refcount by scope alone. Two passes: note what changed, then rebuild — a
 *  rebuild deletes widgets, which mutates s_pills through their delete
 *  callbacks, so nothing may be rebuilt while that vector is being walked. */
void descDispatch(const char* key, const char* val) {
    formApply(key, val);

    for (auto& p : s_pills)
        if (p.key == key) pillApply(p.lbl, val);

    std::vector<CollCtx*> items, cands;
    for (CollCtx* c : s_colls) {
        if (!c->arrayScope.empty() && strncmp(key, c->arrayScope.c_str(),
                                              c->arrayScope.size()) == 0)
            items.push_back(c);
        if (!c->candScope.empty() && strncmp(key, c->candScope.c_str(),
                                             c->candScope.size()) == 0)
            cands.push_back(c);
    }
    for (CollCtx* c : items) collRebuild(c);
    for (CollCtx* c : cands) candRebuild(c);
}

struct AddBtnCtx { CollCtx* c; int which; };
void addBtnFree(lv_event_t* e) { delete static_cast<AddBtnCtx*>(lv_event_get_user_data(e)); }

void onAddButton(lv_event_t* e) {
    auto* a = static_cast<AddBtnCtx*>(lv_event_get_user_data(e));
    formOpen(a->c->d->adds[a->which].form, ItemScope{}, a->c->errKey, a->c->ackKey,
             {}, "", a->c);
}

void onRefreshButton(lv_event_t* e) {
    lcdSettingRunAction(static_cast<const lcd_action_t*>(lv_event_get_user_data(e)));
}

void collDelete(lv_event_t* e) {
    lv_obj_t* box = static_cast<lv_obj_t*>(lv_event_get_target(e));
    for (auto it = s_colls.begin(); it != s_colls.end(); ++it) {
        if ((*it)->box != box) continue;
        CollCtx* c = *it;
        s_colls.erase(it);
        /* A form this collection opened points into this context (the item
         * editor's descriptor, the answer keys) — it must not outlive it. */
        if (s_form && s_form->owner == c) formForceClose();
        if (!c->arrayScope.empty())  subDrop(c->arrayScope);
        if (!c->candScope.empty())   subDrop(c->candScope);
        if (!c->statusScope.empty()) subDrop(c->statusScope);
        /* Leaving the pane stops the scan. The refresh target is a plain key,
         * so clearing it here is the whole "stop scanning on leave" contract —
         * no straddle carries a visibility timer for it. */
        const lcd_candidates_t* cand = c->d->candidates;
        if (cand && cand->refresh && cand->refresh->kind == LCD_ACT_SET && cand->refresh->key)
            storageSet(cand->refresh->key, "0");
        delete c;
        return;
    }
}

}  // namespace

/* ================= public ================= */

void lcdSettingRunAction(const lcd_action_t* act, const char* itemPrefix,
                         const char* idValue) {
    if (!act) return;
    ItemScope sc;
    if (itemPrefix) sc.prefix  = itemPrefix;
    if (idValue)    sc.idValue = idValue;

    switch (act->kind) {
        case LCD_ACT_SET: {
            if (!act->key) return;
            std::string key = subst(act->key, sc);
            std::string val = subst(act->value ? act->value : "1", sc);
            /* An edge write forces a change past the storage actor's dedup: a
             * command flag left set by an attempt that did not complete would
             * otherwise swallow every later press, permanently. */
            if (act->edge) storageSet(key.c_str(), "0");
            storageSet(key.c_str(), val.c_str());
            if (act->reboots) rebootNotice();
            break;
        }
        case LCD_ACT_DIALOG:
            if (act->dialog) dialogOpen(act->dialog, sc);
            break;
        case LCD_ACT_FORM:
            if (act->form) {
                /* A bare form answers on its own sentinel's keys. */
                std::string cmd = subst(act->form->cmd, sc);
                formOpen(act->form, sc, cmd + ".error", cmd + ".done", {}, "");
            }
            break;
    }
}

void lcdSettingsDescReset(void) {
    /* The Settings app is going away; its modals live on lv_layer_top, outside
     * the app's widget tree, so nothing else takes them down. The form first
     * (its teardown drops subscriptions), then whatever overlays remain —
     * dialogs, remove confirmations — whose contexts free from their own
     * delete callbacks. The on-screen-keyboard editor rides its own overlay. */
    formForceClose();
    kbClose(false);
    while (!s_modals.empty()) {
        lv_obj_t* ov = s_modals.back();
        s_modals.pop_back();
        lv_obj_delete(ov);
    }
}

lv_obj_t* lcdSettingAction(lv_obj_t* parent, const char* label, const lcd_action_t* act) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, [](lv_event_t* e) {
        lcdSettingRunAction(static_cast<const lcd_action_t*>(lv_event_get_user_data(e)));
    }, LV_EVENT_CLICKED, (void*)act);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return b;
}

lv_obj_t* lcdSettingCollection(lv_obj_t* parent, const lcd_collection_t* d) {
    if (!d) return nullptr;
    if (d->label && *d->label) lcdSettingSection(parent, d->label);

    CollCtx* c = new CollCtx();
    c->d      = d;
    c->setCmd = std::string(d->cmd) + ".set";
    c->errKey = std::string(d->cmd) + ".error";
    c->ackKey = std::string(d->cmd) + ".done";
    c->editForm = lcd_form_t{ .fields = d->edit, .nfields = d->nedit,
                              .cmd = c->setCmd.c_str(), .submit = "Save",
                              .title = (d->label && *d->label) ? d->label : nullptr };

    c->box = lv_obj_create(parent);
    lv_obj_remove_style_all(c->box);
    lv_obj_set_width(c->box, lv_pct(100));
    lv_obj_set_height(c->box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c->box, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c->box, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < d->nadds; i++) {
        auto* a = new AddBtnCtx{ c, i };
        lv_obj_t* b = wideButton(parent, d->adds[i].label, false, onAddButton, a);
        lv_obj_add_event_cb(b, addBtnFree, LV_EVENT_DELETE, a);
    }

    if (d->candidates) {
        const lcd_candidates_t* cand = d->candidates;
        if (cand->refresh_label && *cand->refresh_label && cand->refresh)
            wideButton(parent, cand->refresh_label, false, onRefreshButton,
                       (void*)cand->refresh);
        c->candBox = lv_obj_create(parent);
        lv_obj_remove_style_all(c->candBox);
        lv_obj_set_width(c->candBox, lv_pct(100));
        lv_obj_set_height(c->candBox, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(c->candBox, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(c->candBox, LV_OBJ_FLAG_SCROLLABLE);
        c->candScope = cand->key;
    }

    c->arrayScope = d->key;
    if (d->status && *d->status) c->statusScope = literalPrefix(d->status);

    s_colls.push_back(c);
    subAdd(c->arrayScope);
    if (!c->candScope.empty())   subAdd(c->candScope);
    if (!c->statusScope.empty()) subAdd(c->statusScope);
    lv_obj_add_event_cb(c->box, collDelete, LV_EVENT_DELETE, nullptr);

    collRebuild(c);
    if (c->candBox) candRebuild(c);
    return c->box;
}
