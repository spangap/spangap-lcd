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
#include "timezones.h"
#include "log.h"

#include <cJSON.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace {

/* A collection's rows run 20% below the pane's body size (14 px in the shipped
 * sheet): a list is a block of the device's own data rather than more furniture,
 * and it reads as one when everything in it — titles, subtitles, status pills,
 * per-item buttons — sits a step under the fixed rows around it. */
inline int listTextPx() { return lcdPx(11); }
inline int listSubPx()  { return lcdPx(10); }

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

void notice(const char* text);   /* fwd — the one-line modal, with the others */

/** The characters an integer field will take at all: a sign only where the
 *  range actually goes below zero. */
const char* numAccepts(const lcd_num_t* n) {
    return (n->has_min && lcdSettingsNumMin(n) >= 0) ? "0123456789" : "0123456789-";
}

/** The one sentence a refused address gets, wherever it is refused. */
const char* const IPV4_REASON = "Enter an address like 192.168.1.10, or leave it empty.";

/** Commit a typed address to a form field, or refuse it. Empty is accepted:
 *  an address left blank is unset, which is how a fixed IP goes back to DHCP. */
bool ipv4Commit(const std::string& text, std::string& out) {
    if (!lcdSettingsIpv4Ok(text.c_str())) { notice(IPV4_REASON); return false; }
    out = text;
    return true;
}

/** Commit a typed number to a form field, or refuse it. False means refused —
 *  the notice is already up and the caller leaves the editor as it was. */
bool numCommit(const lcd_num_t* n, const std::string& text, std::string& out) {
    int v = 0;
    if (lcdSettingsNumParse(text.c_str(), &v) && lcdSettingsNumOk(n, v)) {
        out = std::to_string(v);
        return true;
    }
    char msg[96];
    lcdSettingsNumRangeMsg(n, msg, sizeof(msg));
    notice(msg);
    return false;
}

/* ---- shared modal scaffolding ---- */

/* Open overlays are tracked by the panel (lcdModalTrack), which unwinds them on
 * the double-tap escape and hands lcdSettingsDescReset() its own teardown —
 * they live on lv_layer_top, outside the app's widget tree, so nothing else
 * would take them down when Settings goes away. */

/** Full-screen dim overlay carrying a centred card. Returns the card (build the
 *  content into it); `*overlayOut` is what to delete to dismiss.
 *
 *  `closeCb` puts a small Close in the card's top-right corner, on the title's
 *  line, instead of the usual button at the foot. For a card that is one long
 *  list — a scan's results — a full row of buttons under it is a row of the
 *  list, and the corner is where a dismiss is looked for anyway. */
lv_obj_t* makeModal(lv_obj_t** overlayOut, const char* title,
                    lv_event_cb_t closeCb = nullptr, void* closeUd = nullptr) {
    /* Read the focus ring's position BEFORE this modal's own widgets join the
     * group, and hand it back when the modal goes. */
    lv_obj_t* opener = lcdInputGroup() ? lv_group_get_focused(lcdInputGroup()) : nullptr;
    lv_obj_t* ov = lv_obj_create(lv_layer_top());
    lcdModalTrack(ov);
    lcdSettingsRefocusOnClose(ov, opener);
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

    if ((title && *title) || closeCb) {
        lv_obj_t* head = lv_obj_create(card);
        lv_obj_remove_style_all(head);
        lv_obj_set_width(head, lv_pct(100));
        lv_obj_set_height(head, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(head, 8, 0);
        lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* t = lv_label_create(head);
        lv_label_set_text(t, (title && *title) ? title : "");
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(t, 1);          /* pushes Close to the far corner */
        lv_obj_set_style_text_color(t, lv_color_white(), 0);
        lv_obj_set_style_text_font(t, lcdFont(LcdFace::UI_BOLD, lcdPx(16)), 0);

        if (closeCb) {
            lv_obj_t* b = lv_button_create(head);
            lv_obj_set_style_pad_hor(b, 6, 0);
            lv_obj_set_style_pad_ver(b, 1, 0);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, "Close");
            lv_obj_set_style_text_font(l, lcdFont(LcdFace::UI, lcdPx(11)), 0);
            lv_obj_center(l);
            lv_obj_add_event_cb(b, closeCb, LV_EVENT_CLICKED, closeUd);
            if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
        }
    }
    *overlayOut = ov;
    return card;
}

lv_color_t pillColor(const std::string& name);   /* fwd — one palette, below */

/** Paint a button from that palette. A colour a button states and a colour a
 *  pill states are the same word resolved through the same table, so a red
 *  button is the red a red pill is. */
void buttonColor(lv_obj_t* b, const char* color) {
    if (color && *color) lv_obj_set_style_bg_color(b, pillColor(color), 0);
}

/** A bar of buttons: content-sized, gathered right, wrapping when a line of them
 *  does not fit — the way the browser puts a dialog's actions. At the foot of a
 *  modal it goes INSIDE the scrolling body rather than pinned under it, because
 *  on a panel this small a fixed footer is a third of the dialog and it is the
 *  fields that want the space. A pane uses it for a collection's own buttons. */
lv_obj_t* buttonBar(lv_obj_t* parent) {
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_set_style_pad_row(r, 4, 0);
    lv_obj_set_style_pad_top(r, 4, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/** One button of such a bar: sized to its label, not to the dialog. */
lv_obj_t* barButton(lv_obj_t* row, const char* label, const char* color,
                    lv_event_cb_t cb, void* ud) {
    lv_obj_t* b = lv_button_create(row);
    lcdSettingsHalfPadVer(b);
    buttonColor(b, color);
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
    lcdModalUntrack(overlay);
    lv_obj_delete_async(overlay);
}

/* ---- refcounted storage subscriptions ----
 * One dispatcher (descDispatch) serves every subscriber in this file, so a scope
 * needs exactly one real subscription however many users it has — and the
 * refcount is PER SCOPE, not per (scope, callback): with per-pair counts, a form
 * closing its watch on a collection's array would drop the scope out from under
 * the collection's own live count.
 *
 * The drop goes through storageUnsubscribeCb, so it takes down this file's
 * dispatcher and nothing else. Anything else on the lcd task watching the same
 * scope — the plain-row binding table in lcd_settings.cpp, a module's own live
 * watch on one of its keys — keeps its subscription. */

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
        storageUnsubscribeCb(scope.c_str(), descDispatch);
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
    lv_obj_set_style_text_font(l, lcdFont(LcdFace::UI, listTextPx()), 0);
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
    lv_obj_t* rowObj2 = nullptr;    /* timezone rows span two rows: region, zone */
    lv_obj_t* widget2 = nullptr;    /* the zone dropdown */
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
    int  itemIdx   = -1;            /* >= 0: an item detail page of that collection */
    bool submitted = false;
};
FormCtx* s_form = nullptr;

/** What `{name}` means inside a form: a field it carries, else — for an item
 *  editor — the item's own stored value. The fallback is what lets a detail page
 *  name the thing it is editing ("{ssid}" in a heading) without also having to
 *  offer it as a row. Empty for a bare form, which has no item behind it. */
std::string formLookup(const std::string& name) {
    if (!s_form) return "";
    for (auto& f : s_form->fields) {
        const char* fn = f.row->field;
        if (fn && name == fn) return f.value;
    }
    return fieldOf(s_form->sc, name);
}

bool headingRow(const lcd_row_t* r);   /* fwd: a section / caption row */
/* fwd: an item detail page's per-item buttons (defined with the collections,
 * since they are the collection's, not the form's) */
void buildItemButtons(lv_obj_t* row, FormCtx* ctx);
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

/* ---- timezone picker data ----
 * An LCD_ROW_TIMEZONE field renders as two dropdowns — region, then zone —
 * fed from the firmware's built-in zone table (timezones.h): rodata arrays,
 * nothing read from disk and nothing parsed. Only the newline-joined option
 * strings the lv_dropdowns copy are transient heap. Both dropdowns show
 * exactly what they store, so the live widget's own option string is the
 * value source and nothing else needs to be kept. */

/** Newline-joined regions: the unique first path segments of TZ_NAMES. The
 *  table is name-sorted, so equal regions are adjacent. */
std::string tzRegionOptions() {
    std::string out, last;
    for (int i = 0; i < TZ_COUNT; i++) {
        const char* slash = strchr(TZ_NAMES[i], '/');
        std::string region = slash ? std::string(TZ_NAMES[i], slash - TZ_NAMES[i])
                                   : TZ_NAMES[i];
        if (region == last) continue;
        if (!out.empty()) out += '\n';
        out += region;
        last = region;
    }
    return out;
}

/** Newline-joined zones of one region — the name minus the region prefix, so
 *  a nested country level reads "Argentina/Buenos_Aires". */
std::string tzZoneList(const std::string& region) {
    std::string prefix = region + "/";
    std::string out;
    for (int i = 0; i < TZ_COUNT; i++) {
        if (strncmp(TZ_NAMES[i], prefix.c_str(), prefix.size()) != 0) continue;
        if (!out.empty()) out += '\n';
        out += TZ_NAMES[i] + prefix.size();
    }
    return out;
}

/** Select the entry of a live dropdown whose text equals `want`. */
bool ddSelectText(lv_obj_t* dd, const std::string& want) {
    std::string all(lv_dropdown_get_options(dd) ? lv_dropdown_get_options(dd) : "");
    size_t start = 0;
    for (uint32_t i = 0; ; i++) {
        size_t nl = all.find('\n', start);
        std::string one = all.substr(start, nl == std::string::npos ? nl : nl - start);
        if (one == want) { lv_dropdown_set_selected(dd, i); return true; }
        if (nl == std::string::npos) return false;
        start = nl + 1;
    }
}

/** The dropdown's currently selected text. */
std::string ddSelectedText(lv_obj_t* dd) {
    return nthOption(lv_dropdown_get_options(dd), lv_dropdown_get_selected(dd));
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
        case LCD_ROW_TIMEZONE: {
            if (!f.widget || !f.widget2) break;
            size_t slash = f.value.find('/');
            std::string region = slash == std::string::npos ? f.value : f.value.substr(0, slash);
            std::string zone   = slash == std::string::npos ? "" : f.value.substr(slash + 1);
            if (!region.empty() && ddSelectText(f.widget, region)) {
                std::string zones = tzZoneList(region);
                lv_dropdown_set_options(f.widget2, zones.c_str());
                ddSelectText(f.widget2, zone);
            }
            break;
        }
        case LCD_ROW_INTEGER:
            /* Unlike a text field, this one IS refreshed while focused: the
             * +/- buttons move it under the caret and the field has to follow
             * them. Nothing else writes it — the operator's own typing already
             * IS the value. */
            if (f.valLbl) lv_label_set_text(f.valLbl, f.value.c_str());
            else if (f.widget) lv_textarea_set_text(f.widget, f.value.c_str());
            break;
        case LCD_ROW_IPV4:
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
        if (headingRow(f.row) && f.rowObj && f.row->label && strchr(f.row->label, '{'))
            lv_label_set_text(f.rowObj, substWith(f.row->label, formLookup).c_str());
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
            if (f.rowObj2) {   /* a timezone field's second row gates with it */
                if (on) lv_obj_remove_flag(f.rowObj2, LV_OBJ_FLAG_HIDDEN);
                else    lv_obj_add_flag   (f.rowObj2, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* -- the on-screen-keyboard editor for one form field -- */
struct KbCtx { FormField* f; lv_obj_t* overlay; lv_obj_t* ta; };
KbCtx s_kb;

void kbClose(bool commit) {
    if (commit && s_kb.f) {
        /* A number outside the field's range is refused rather than clamped:
         * clamping stores something nobody typed. The editor stays up behind
         * the notice with what was typed still in it. */
        if (s_kb.f->row->kind == LCD_ROW_INTEGER && s_kb.f->row->num) {
            if (!numCommit(s_kb.f->row->num, lv_textarea_get_text(s_kb.ta),
                           s_kb.f->value))
                return;
        } else if (s_kb.f->row->kind == LCD_ROW_IPV4) {
            if (!ipv4Commit(lv_textarea_get_text(s_kb.ta), s_kb.f->value)) return;
        } else {
            s_kb.f->value = lv_textarea_get_text(s_kb.ta);
        }
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
    /* Its own closer: a bare delete would leave s_kb pointing at freed
     * widgets, and the escape must not commit what was half-typed. */
    lcdModalTrack(ov, [](void*) { kbClose(false); });

    lv_obj_t* ta = lv_textarea_create(ov);
    lcdSettingsHalfPadVer(ta);
    lv_obj_set_size(ta, lv_pct(96), 56);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 6);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, f->row->secret);
    if (f->row->kind == LCD_ROW_INTEGER && f->row->num)
        lv_textarea_set_accepted_chars(ta, numAccepts(f->row->num));
    else if (f->row->kind == LCD_ROW_IPV4)
        lv_textarea_set_accepted_chars(ta, "0123456789.");
    if (f->row->placeholder_key && *f->row->placeholder_key)
        lv_textarea_set_placeholder_text(
            ta, storageGetStr(f->row->placeholder_key, "").c_str());
    else if (f->row->placeholder)
        lv_textarea_set_placeholder_text(ta, f->row->placeholder);
    lv_textarea_set_text(ta, f->value.c_str());
    s_kb.ta = ta;

    lv_obj_t* kb = lv_keyboard_create(ov);
    if (f->row->kind == LCD_ROW_INTEGER || f->row->kind == LCD_ROW_IPV4)
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, kbEvent, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(kb, kbEvent, LV_EVENT_CANCEL, nullptr);
}

void onFieldInline(lv_event_t* e) {          /* physical keyboard: edit in place */
    auto* f = static_cast<FormField*>(lv_event_get_user_data(e));
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (f->row->kind == LCD_ROW_INTEGER && f->row->num) {
        /* Refused: back to the value the field held, so it never sits showing
         * a number that is not the one this form will submit. */
        if (!numCommit(f->row->num, lv_textarea_get_text(ta), f->value)) {
            lv_textarea_set_text(ta, f->value.c_str());
            return;
        }
    } else if (f->row->kind == LCD_ROW_IPV4) {
        if (!ipv4Commit(lv_textarea_get_text(ta), f->value)) {
            lv_textarea_set_text(ta, f->value.c_str());
            return;
        }
    } else {
        f->value = lv_textarea_get_text(ta);
    }
    f->dirty = true;
    formRefresh();
}

/** One press of a form field's + or -, over the local buffer rather than
 *  storage: nothing in a form reaches the device before submit. */
struct FieldStep { FormField* f; int dir; };
void fieldStepFree(lv_event_t* e) { delete static_cast<FieldStep*>(lv_event_get_user_data(e)); }

void onFieldNumStep(lv_event_t* e) {
    auto* s = static_cast<FieldStep*>(lv_event_get_user_data(e));
    FormField* f = s->f;
    const lcd_num_t* n = f->row->num;
    if (!n) return;
    int cur = 0;
    if (!lcdSettingsNumParse(f->value.c_str(), &cur))
        cur = n->has_min ? lcdSettingsNumMin(n) : 0;
    f->value = std::to_string(lcdSettingsNumStep(n, cur, s->dir));
    f->dirty = true;
    formApplyValue(*f);
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

void onFieldTzRegion(lv_event_t* e) {
    auto* f = static_cast<FormField*>(lv_event_get_user_data(e));
    std::string region = ddSelectedText(static_cast<lv_obj_t*>(lv_event_get_target_obj(e)));
    std::string zones = tzZoneList(region);
    lv_dropdown_set_options(f->widget2, zones.c_str());
    lv_dropdown_set_selected(f->widget2, 0);
    std::string zone = nthOption(zones.c_str(), 0);
    f->value = zone.empty() ? region : region + "/" + zone;
    f->dirty = true;
    formRefresh();
}

void onFieldTzZone(lv_event_t* e) {
    auto* f = static_cast<FormField*>(lv_event_get_user_data(e));
    std::string region = ddSelectedText(f->widget);
    std::string zone = ddSelectedText(static_cast<lv_obj_t*>(lv_event_get_target_obj(e)));
    f->value = zone.empty() ? region : region + "/" + zone;
    f->dirty = true;
    formRefresh();
}

/** A form's own headings template like everything else does, against the local
 *  field buffer: "SSID: {ssid}" over the item being edited. Kept live by
 *  formRefresh, so a heading naming a field the operator is editing follows it. */
bool headingRow(const lcd_row_t* r) {
    return r->kind == LCD_ROW_TITLE || r->kind == LCD_ROW_HEADING
        || r->kind == LCD_ROW_SECTION || r->kind == LCD_ROW_CAPTION;
}

/** A word after the field — a unit, or the fixed tail of what is entered.
 *  Furniture the operator reads, never part of the value. */
bool hasUnit(const lcd_row_t* r) { return r->unit && *r->unit; }

void fieldUnit(lv_obj_t* row, const lcd_row_t* r) {
    if (!hasUnit(r)) return;
    lv_obj_t* u = lv_label_create(row);
    lv_label_set_text(u, r->unit);
    lv_obj_set_style_text_color(u, lv_color_hex(0x8a93a0), 0);
}

/** A third of the usual width, where the row asks for it. A unit does NOT
 *  imply it — a subdomain with ".duckdns.org" after it still needs room for a
 *  subdomain. */
bool fieldNarrow(const lcd_row_t* r) { return r->short_; }

void narrowField(lv_obj_t* w) {
    lv_obj_set_flex_grow(w, 0);
    lv_obj_set_width(w, lv_pct(30));
}

void buildFormField(lv_obj_t* parent, FormField& f, bool underHeading) {
    const lcd_row_t* r = f.row;
    if (headingRow(r)) {
        std::string txt = substWith(r->label, formLookup);
        f.rowObj = (r->kind == LCD_ROW_CAPTION)
            ? lcdSettingCaption(parent, txt.c_str(), underHeading)
            : lcdSettingSection(parent, txt.c_str());
        return;
    }

    /* Compact rows: a modal is a window onto a pane, and the fewer of its rows
     * fit the more it scrolls for content that would have fitted. */
    lv_obj_t* row = lcdSettingsMakeRow(parent, /*compact=*/true);
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
        case LCD_ROW_INTEGER: {
            static const lcd_num_t unbounded{};
            const lcd_num_t* n = r->num ? r->num : &unbounded;

            /* The field between its steppers, the same shape the pane's own
             * integer row has — a form field and a pane row must be the same
             * control or the operator is learning two of them. */
            lv_obj_t* grp = lv_obj_create(row);
            lv_obj_remove_style_all(grp);
            lv_obj_set_height(grp, LV_SIZE_CONTENT);
            lcdSettingsFillControl(grp);
            lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(grp, 6, 0);
            lv_obj_remove_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

            auto stepper = [&](const char* glyph, int dir) {
                lv_obj_t* b = lv_button_create(grp);
                lv_obj_set_style_pad_hor(b, 8, 0);
                lv_obj_set_style_pad_ver(b, 1, 0);
                lv_obj_t* l = lv_label_create(b);
                lv_label_set_text(l, glyph);
                lv_obj_center(l);
                auto* s = new FieldStep{ &f, dir };
                lv_obj_add_event_cb(b, onFieldNumStep, LV_EVENT_CLICKED, s);
                lv_obj_add_event_cb(b, fieldStepFree, LV_EVENT_DELETE, s);
                if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
            };
            if (lcdHasKeyboard()) {
                lv_obj_t* ta = lv_textarea_create(grp);
                lv_textarea_set_one_line(ta, true);
                lv_textarea_set_accepted_chars(ta, numAccepts(n));
                lv_textarea_set_text(ta, f.value.c_str());
                lv_obj_set_flex_grow(ta, 1);
                lcdSettingsHalfPadVer(ta);
                if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), ta);
                f.widget = ta;
                lv_obj_add_event_cb(ta, onFieldInline, LV_EVENT_READY,     &f);
                lv_obj_add_event_cb(ta, onFieldInline, LV_EVENT_DEFOCUSED, &f);
            } else {
                lv_obj_t* val = lv_label_create(grp);
                lv_obj_set_style_text_color(val, lv_color_hex(0xb0b8c0), 0);
                lv_obj_set_flex_grow(val, 1);
                lv_label_set_text(val, f.value.c_str());
                f.valLbl = val;
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(row, onFieldTap, LV_EVENT_CLICKED, &f);
            }
            fieldUnit(grp, r);
            if (n->buttons) { stepper("-", -1); stepper("+", +1); }
            break;
        }
        case LCD_ROW_DROPDOWN: {
            lv_obj_t* d = lv_dropdown_create(row);
            lv_obj_set_width(d, LV_SIZE_CONTENT);      /* sized to its options */
            lv_obj_set_style_max_width(d, lv_pct(66), 0);
            lcdSettingsHalfPadVer(d);
            /* Show the labels, store the values — the list is for a person. */
            lv_dropdown_set_options(d, (r->opt_labels && *r->opt_labels) ? r->opt_labels
                                                                        : r->options);
            f.widget = d;
            lv_obj_add_event_cb(d, onFieldDropdown, LV_EVENT_VALUE_CHANGED, &f);
            break;
        }
        case LCD_ROW_TIMEZONE: {
            /* A picker's "value if left alone" is its current selection: seed
             * from the applied zone so opening and submitting is a no-op edit. */
            if (f.value.empty() && r->placeholder_key && *r->placeholder_key)
                f.value = storageGetStr(r->placeholder_key, "");

            std::string regions = tzRegionOptions();

            lv_obj_t* d1 = lv_dropdown_create(row);
            lcdSettingsFillControl(d1);
            lcdSettingsHalfPadVer(d1);
            lv_dropdown_set_options(d1, regions.c_str());
            f.widget = d1;
            lv_obj_add_event_cb(d1, onFieldTzRegion, LV_EVENT_VALUE_CHANGED, &f);

            /* The zone dropdown gets a second, label-less row of its own —
             * two full-width lists read better than two crammed halves. */
            lv_obj_t* row2 = lcdSettingsMakeRow(parent, /*compact=*/true);
            lcdSettingsRowLabel(row2, "");
            f.rowObj2 = row2;
            lv_obj_t* d2 = lv_dropdown_create(row2);
            lcdSettingsFillControl(d2);
            lcdSettingsHalfPadVer(d2);
            lv_dropdown_set_options(d2, "");   /* never LVGL's sample options */
            f.widget2 = d2;
            lv_obj_add_event_cb(d2, onFieldTzZone, LV_EVENT_VALUE_CHANGED, &f);

            /* No seed (or one formApplyValue can't match): show the first
             * region's zones and make the visible selection the value, so
             * what the operator sees is always what submit sends. */
            if (f.value.empty()) {
                std::string region = nthOption(regions.c_str(), 0);
                std::string zones = tzZoneList(region);
                lv_dropdown_set_options(d2, zones.c_str());
                std::string zone = nthOption(zones.c_str(), 0);
                if (!region.empty())
                    f.value = zone.empty() ? region : region + "/" + zone;
            }
            break;
        }
        case LCD_ROW_IPV4:
        case LCD_ROW_TEXT:
            if (lcdHasKeyboard()) {
                lv_obj_t* ta = lv_textarea_create(row);
                lv_textarea_set_one_line(ta, true);
                lv_textarea_set_password_mode(ta, r->secret);
                if (r->kind == LCD_ROW_IPV4)
                    lv_textarea_set_accepted_chars(ta, "0123456789.");
                /* A placeholder the device publishes outranks the compiled-in
                 * one: it is the value this field would take if left empty,
                 * which only the firmware knows (a MAC, a default port). */
                if (r->placeholder_key && *r->placeholder_key)
                    lv_textarea_set_placeholder_text(
                        ta, storageGetStr(r->placeholder_key, "").c_str());
                else if (r->placeholder)
                    lv_textarea_set_placeholder_text(ta, r->placeholder);
                if (fieldNarrow(r)) narrowField(ta); else lcdSettingsFillControl(ta);
                if (hasUnit(r)) lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_RIGHT, 0);
                lcdSettingsHalfPadVer(ta);
                if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), ta);
                f.widget = ta;
                lv_obj_add_event_cb(ta, onFieldInline, LV_EVENT_READY,     &f);
                lv_obj_add_event_cb(ta, onFieldInline, LV_EVENT_DEFOCUSED, &f);
            } else {
                lv_obj_t* val = lv_label_create(row);
                lv_obj_set_style_text_color(val, lv_color_hex(0xb0b8c0), 0);
                if (fieldNarrow(r)) narrowField(val); else lcdSettingsFillControl(val);
                lv_obj_set_style_text_align(
                    val, hasUnit(r) ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
                f.valLbl = val;
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(row, onFieldTap, LV_EVENT_CLICKED, &f);
            }
            fieldUnit(row, r);
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
    if (f->overlay) lv_obj_delete(f->overlay);   /* untracks itself on delete */
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
 *  add; `owner` ties the form to the collection whose teardown closes it, and
 *  `itemIdx >= 0` makes it that collection's ITEM DETAIL PAGE — the fields plus
 *  everything the item can be made to do, which is why a list row now carries no
 *  buttons of its own. */
void formOpen(const lcd_form_t* d, const ItemScope& sc,
              const std::string& errKey, const std::string& ackKey,
              const std::vector<std::pair<std::string, std::string>>& prefill,
              const std::string& editId, const void* owner = nullptr,
              int itemIdx = -1) {
    if (s_form) formForceClose();
    FormCtx* ctx = new FormCtx();
    ctx->d        = d;
    ctx->sc       = sc;
    ctx->cmdKey   = subst(d->cmd, sc);
    ctx->errKey   = errKey;
    ctx->ackKey   = ackKey;
    ctx->editId   = editId;
    ctx->owner    = owner;
    ctx->itemIdx  = itemIdx;
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
    /* The form is the one dialog whose overlay is not the whole of it: the
     * escape must drop its storage subscriptions and the s_form pointer, not
     * just the widgets. */
    lcdModalTrack(ctx->overlay, [](void*) { formForceClose(); });
    /* One scroll container holding everything below the title — fields, the
     * rejection line, and the buttons. Nothing is pinned: on a panel this size
     * a fixed footer costs more than scrolling past it does. */
    lv_obj_t* body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(body, (lcdScreenH() * 2) / 3, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 4, 0);
    /* A step under the pane's body size: the dialog is a detail view, and its
     * rows are already compact ones. */
    lv_obj_set_style_text_font(body, lcdFont(LcdFace::UI, lcdPx(12)), 0);

    /* A caption directly under a heading is about the group and spans both
     * columns; one under a field starts on the control column. Only the order
     * of the rows knows which, so the walk carries it. */
    bool afterHeading = false;
    for (auto& f : ctx->fields) {
        buildFormField(body, f, afterHeading);
        lcd_row_kind_t k = f.row->kind;
        if (k == LCD_ROW_TITLE || k == LCD_ROW_HEADING || k == LCD_ROW_SECTION)
            afterHeading = true;
        else if (k != LCD_ROW_CAPTION)
            afterHeading = false;
    }

    ctx->errLbl = lv_label_create(body);
    lv_label_set_long_mode(ctx->errLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->errLbl, lv_pct(100));
    lv_obj_set_style_text_color(ctx->errLbl, lv_color_hex(0xe06c6c), 0);
    /* Hidden until a verdict arrives; submit clears the device-side reason, so
     * a stale one from an earlier attempt is never shown. */
    lv_obj_add_flag(ctx->errLbl, LV_OBJ_FLAG_HIDDEN);

    /* Left to right: what the item can be made to do, then leaving, then
     * committing. Destructive first and primary last, so the rightmost button
     * — the one under a thumb reaching from the edge — is the safe one. */
    lv_obj_t* btns = buttonBar(body);
    buildItemButtons(btns, ctx);
    barButton(btns, "Cancel", nullptr, onFormCancel, nullptr);
    barButton(btns, (d->submit && *d->submit) ? d->submit : "Save", nullptr,
                onFormSubmit, nullptr);

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

    lv_obj_t* btns = buttonBar(card);
    for (int i = 0; i < d->nbuttons; i++) {
        auto* c = new DlgBtnCtx{ ov, d->buttons[i].act, sc };
        lv_obj_t* b = barButton(btns, d->buttons[i].label, d->buttons[i].color,
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

/** One line and an OK: how a refused number tells the operator why. Not a
 *  dialog descriptor — there is no choice to make and nothing to run, so it
 *  takes text rather than an lcd_dialog_t. */
void notice(const char* text) {
    lv_obj_t* ov = nullptr;
    lv_obj_t* card = makeModal(&ov, nullptr);
    lv_obj_t* t = lv_label_create(card);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text(t, text);

    lv_obj_t* btns = buttonBar(card);
    barButton(btns, "OK", nullptr, [](lv_event_t* e) {
        dismiss(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
    }, ov);
}

/* ---- collections ---- */

struct CollCtx {
    const lcd_collection_t* d;
    lv_obj_t* box     = nullptr;    /* item rows */
    lv_obj_t* candBox = nullptr;    /* candidate rows, inside the scan popup */
    std::string arrayScope, candScope, statusScope;
    lv_obj_t* candOverlay = nullptr;   /* the scan popup, while it is open */
    /* Where every one of this collection's sentinels answers. */
    std::string errKey, ackKey;
    /* The item editor, assembled once from `edit:` + `<cmd>.set`. It has to
     * outlive the click that opens it, and there is one per collection. */
    lcd_form_t  editForm{};
    std::string setCmd;
    /* The detail page's heading, rebuilt for each item as it is opened. It
     * backs editForm.title, so it has to outlive the click that opens the
     * form — which it does, being a member alongside it. */
    std::string editTitle;
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
    lv_obj_t* btns = buttonBar(card);
    barButton(btns, "Remove", "red", [](lv_event_t* e) {
        auto* r = static_cast<RmCtx*>(lv_event_get_user_data(e));
        std::string k = r->key, id = r->id;      /* the dismiss frees r */
        dismiss(r->ov);
        storageSet(k.c_str(), id.c_str());
    }, rc);
    barButton(btns, "Cancel", nullptr, [](lv_event_t* e) {
        dismiss(static_cast<RmCtx*>(lv_event_get_user_data(e))->ov);
    }, rc);
}

void collRebuild(CollCtx* c);
void candRebuild(CollCtx* c);
void candPopupClose(CollCtx* c);   /* the scan popup; below with the scan button */

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

/** The current id order of the array with one item MOVED to a new position,
 *  comma-joined — what a drag writes. A move, not a swap: dragging a row three
 *  places up puts it there and closes the gap behind it, which is what the row
 *  under the finger has been doing all the way. */
std::string idOrder(const lcd_collection_t* d, int moveFrom, int moveTo) {
    int n = storageArrayCount(d->key);
    std::vector<std::string> ids;
    for (int i = 0; i < n; i++) ids.push_back(itemScope(d, i).idValue);
    if (moveFrom >= 0 && moveFrom < n && moveTo >= 0 && moveTo < n && moveFrom != moveTo) {
        std::string moved = ids[moveFrom];
        ids.erase(ids.begin() + moveFrom);
        ids.insert(ids.begin() + moveTo, moved);
    }
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
    bool remove;
    bool edit;
};
void itemBtnFree(lv_event_t* e) { delete static_cast<ItemBtnCtx*>(lv_event_get_user_data(e)); }

void onItemButton(lv_event_t* e) {
    auto* b = static_cast<ItemBtnCtx*>(lv_event_get_user_data(e));
    const lcd_collection_t* d = b->c->d;
    ItemScope sc = itemScope(d, b->idx);

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
        /* The item's detail page: the collection's `edit:` rows over
         * `<cmd>.set` — the sentinel family stays derived from the one `cmd`
         * name, so the descriptor never spells the keys out — plus, from
         * buildItemButtons, everything else the item can be made to do. */
        b->c->editTitle = subst(d->item, sc);
        b->c->editForm.title = b->c->editTitle.c_str();
        formOpen(&b->c->editForm, sc, b->c->errKey, b->c->ackKey, {}, sc.idValue,
                 b->c, b->idx);
        return;
    }
    if (b->act) lcdSettingRunAction(b->act, sc.prefix.c_str(), sc.idValue.c_str());
}

/* -- the detail page's own buttons --
 * A per-item action and removal both leave the item's detail page behind: the
 * action may make the page's own contents stale (a network you just connected
 * to), and a removal takes the item out from under it. So each closes the page
 * first and then runs, which also puts a confirmation dialog on a clear screen
 * instead of stacking it on the page it is about to invalidate. */
struct DetailBtnCtx { CollCtx* c; int idx; const lcd_action_t* act; bool remove; };

void detailBtnFree(lv_event_t* e) { delete static_cast<DetailBtnCtx*>(lv_event_get_user_data(e)); }

void onDetailButton(lv_event_t* e) {
    auto* b = static_cast<DetailBtnCtx*>(lv_event_get_user_data(e));
    const lcd_collection_t* d = b->c->d;
    ItemScope sc  = itemScope(d, b->idx);       /* copies: the close frees b */
    const lcd_action_t* act = b->act;
    bool remove   = b->remove;
    std::string rmKey = std::string(d->cmd) + ".remove";
    std::string confirm = remove && d->remove_confirm && *d->remove_confirm
                        ? subst(d->remove_confirm, sc) : "";

    formClose();
    if (!remove) {
        if (act) lcdSettingRunAction(act, sc.prefix.c_str(), sc.idValue.c_str());
        return;
    }
    if (confirm.empty()) storageSet(rmKey.c_str(), sc.idValue.c_str());
    else                 confirmRemove(confirm, rmKey, sc.idValue);
}

void buildItemButtons(lv_obj_t* row, FormCtx* ctx) {
    if (!ctx || ctx->itemIdx < 0 || !ctx->owner) return;
    CollCtx* c = const_cast<CollCtx*>(static_cast<const CollCtx*>(ctx->owner));
    const lcd_collection_t* d = c->d;
    ItemScope sc = itemScope(d, ctx->itemIdx);

    if (d->has_remove) {
        auto* bc = new DetailBtnCtx{ c, ctx->itemIdx, nullptr, true };
        lv_obj_t* b = barButton(row, "Delete", "red", onDetailButton, bc);
        lv_obj_add_event_cb(b, detailBtnFree, LV_EVENT_DELETE, bc);
    }
    for (int a = 0; a < d->nactions; a++) {
        const lcd_item_action_t& ia = d->actions[a];
        auto* bc = new DetailBtnCtx{ c, ctx->itemIdx, ia.act, false };
        lv_obj_t* b = barButton(row, ia.label, ia.color, onDetailButton, bc);
        lv_obj_add_event_cb(b, detailBtnFree, LV_EVENT_DELETE, bc);
        /* The gate is the item's, so the key templates against it — "Connect"
         * is gone on the network already connected. Resolved and subscribed
         * live, so it goes the moment the device says so. */
        if (ia.when_key && *ia.when_key)
            lcdSettingWhenKey(b, subst(ia.when_key, sc).c_str());
    }
}

/* -- drag to reorder --
 *
 * The grip at the right of a row is what a drag starts on, and only there: a
 * vertical drag anywhere else on a list is the pane scrolling, which is what a
 * finger on a long pane is nearly always doing. The grip keeps that from
 * happening by not chaining its scroll to the page (LVGL looks for the
 * scrollable ancestor of whatever was pressed, and stops at a child that
 * refuses to chain).
 *
 * While dragging, only the dragged row moves — it is translated under the
 * finger and drawn over its neighbours, and the list itself does not shuffle.
 * The landing index is the distance travelled in whole rows; the release
 * writes the whole id order to `<cmd>.order` and the array comes back
 * re-published, which is what redraws the list in its new order. */
struct DragCtx {
    CollCtx*  c;
    int       idx;
    lv_obj_t* row;
    int32_t   startY = 0;
    int32_t   rowH   = 0;
};
void dragCtxFree(lv_event_t* e) { delete static_cast<DragCtx*>(lv_event_get_user_data(e)); }

/** Where the row would land, given how far it has been dragged. */
int dragTarget(const DragCtx* g, int32_t dy) {
    int n = storageArrayCount(g->c->d->key);
    if (g->rowH <= 0 || n <= 0) return g->idx;
    int moved = (int)((dy + (dy >= 0 ? g->rowH / 2 : -g->rowH / 2)) / g->rowH);
    int to = g->idx + moved;
    if (to < 0) to = 0;
    if (to > n - 1) to = n - 1;
    return to;
}

void onGripEvent(lv_event_t* e) {
    auto* g = static_cast<DragCtx*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:
            g->startY = p.y;
            g->rowH   = lv_obj_get_height(g->row);
            lv_obj_move_foreground(g->row);      /* over its neighbours, not under */
            lv_obj_set_style_opa(g->row, LV_OPA_90, 0);
            break;
        case LV_EVENT_PRESSING:
            lv_obj_set_style_translate_y(g->row, p.y - g->startY, 0);
            break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST: {
            int32_t dy = p.y - g->startY;
            lv_obj_set_style_translate_y(g->row, 0, 0);
            lv_obj_set_style_opa(g->row, LV_OPA_COVER, 0);
            int to = dragTarget(g, dy);
            if (to != g->idx)
                storageSet((std::string(g->c->d->cmd) + ".order").c_str(),
                           idOrder(g->c->d, g->idx, to).c_str());
            break;
        }
        default: break;
    }
}

void itemRowGrip(lv_obj_t* row, CollCtx* c, int idx) {
    lv_obj_t* grip = lv_label_create(row);
    lv_label_set_text(grip, LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(grip, lv_color_hex(0x8a93a0), 0);
    lv_obj_set_style_text_font(grip, lcdFont(LcdFace::UI, listTextPx()), 0);
    lv_obj_add_flag(grip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(grip, LV_OBJ_FLAG_SCROLL_CHAIN_VER);   /* drag here ≠ scroll */
    lv_obj_set_ext_click_area(grip, 8);

    auto* g = new DragCtx{ c, idx, row };
    for (lv_event_code_t code : { LV_EVENT_PRESSED, LV_EVENT_PRESSING,
                                  LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST })
        lv_obj_add_event_cb(grip, onGripEvent, code, g);
    lv_obj_add_event_cb(grip, dragCtxFree, LV_EVENT_DELETE, g);
}

/* One entry of a collection. The rows of a list are the device's own data
 * sitting inside the fixed furniture of a settings pane, so they are banded:
 * two dark greys alternating, neutral where the chrome around them is blue-
 * tinted, edge to edge with no gap between rows so the banding reads as one
 * block. `idx` is the row's position in the list — the band it gets. */
lv_obj_t* listRow(lv_obj_t* parent, int idx) {
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(r, 8, 0);
    lv_obj_set_style_pad_ver(r, 6, 0);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex((idx & 1) ? 0x282828 : 0x1e1e1e), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
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
    /* An lv_obj is clickable by default, and this one covers most of the row —
     * it would swallow the tap that is meant to open the item. The labels inside
     * it are labels, which are not. */
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* t = lv_label_create(col);
    lv_label_set_text(t, title.c_str());
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_style_text_font(t, lcdFont(LcdFace::UI, listTextPx()), 0);

    if (!sub.empty()) {
        lv_obj_t* s = lv_label_create(col);
        lv_label_set_text(s, sub.c_str());
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s, lv_pct(100));
        lv_obj_set_style_text_color(s, lv_color_hex(0x8a93a0), 0);
        lv_obj_set_style_text_font(s, lcdFont(LcdFace::MONO, listSubPx()), 0);
    }
    return col;
}

/** Has this collection anything to say about one item beyond its row? If not,
 *  the row is a readout and stays unclickable. */
bool hasDetailPage(const lcd_collection_t* d) {
    return d->nedit > 0 || d->nactions > 0 || d->has_remove;
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
        lv_obj_t* row = listRow(c->box, i);
        titleBlock(row, subst(d->item, sc),
                   (d->subtitle && *d->subtitle) ? subst(d->subtitle, sc) : "");

        if (d->status && *d->status) makePill(row, subst(d->status, sc));

        /* Reordering stays on the row: it is about the row's place in the list,
         * not about the item, and it needs its neighbours in view to make sense.
         * Everything that acts on the ITEM — the editor, the per-item actions,
         * removal — lives on its detail page, which the row opens. A row of
         * five buttons is a row nobody can hit. */
        if (d->reorder) itemRowGrip(row, c, i);
        /* No chevron: a banded block of rows in a pane of chevron-less rows is
         * already visibly a list of things, and the affordance would cost the
         * width the title needs. */
        if (hasDetailPage(d)) {
            auto* ctx = new ItemBtnCtx{ c, i, nullptr, false, true };
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, onItemButton, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(row, itemBtnFree, LV_EVENT_DELETE, ctx);
        }
    }
}

/* -- candidates -- */

struct CandCtx { CollCtx* c; int idx; };
void candCtxFree(lv_event_t* e) { delete static_cast<CandCtx*>(lv_event_get_user_data(e)); }

void onCandidatePick(lv_event_t* e) {
    auto* cc = static_cast<CandCtx*>(lv_event_get_user_data(e));
    CollCtx* c = cc->c;                    /* the close below frees cc's row */
    const lcd_collection_t* d = c->d;
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
    /* Picking one answers the question the scan asked, so the popup goes and the
     * scan with it, and the add form opens over a clear screen. The prefill is
     * already read by here, which is the order that matters. */
    candPopupClose(c);
    formOpen(form, ItemScope{}, c->errKey, c->ackKey, prefill, "", c);
}

void candRebuild(CollCtx* c) {
    if (!c->candBox) return;
    lv_obj_clean(c->candBox);
    const lcd_candidates_t* cand = c->d->candidates;
    int n = storageArrayCount(cand->key);
    if (n == 0) { lcdSettingCaption(c->candBox, "Scanning\xE2\x80\xA6"); return; }
    for (int i = 0; i < n; i++) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s.%d", cand->key, i);
        ItemScope sc;
        sc.prefix = buf;
        lv_obj_t* row = listRow(c->candBox, i);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        titleBlock(row, subst(cand->item, sc),
                   (cand->subtitle && *cand->subtitle) ? subst(cand->subtitle, sc) : "");
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

/* -- the scan popup --
 * Candidates are what the device can SEE, which is a different question from
 * what it is configured for, and a transient answer to it: they arrive over
 * seconds, they change, and they are gone the moment you stop asking. Rendered
 * into the pane they push the configured list around while you are reading it,
 * and land under a button that may be most of a screen below the fold. So the
 * scan button opens them as a popup — the results are the whole screen, they
 * start at the top of it, and closing it is what stops the scan. */

/** Stop the scan. The refresh target is a plain key, so clearing it is the whole
 *  "stop scanning" contract — no straddle carries a timer for it. */
void candScanStop(const lcd_collection_t* d) {
    const lcd_candidates_t* cand = d->candidates;
    if (cand && cand->refresh && cand->refresh->kind == LCD_ACT_SET && cand->refresh->key)
        storageSet(cand->refresh->key, "0");
}

/** The popup went away by some path other than candPopupClose — a reset while
 *  the Settings app closes. Found by overlay rather than by a captured pointer:
 *  the deletes are async, so a context that has already been torn down must not
 *  be reachable from one, and a collection drops out of s_colls before it dies. */
void onCandPopupDelete(lv_event_t* e) {
    lv_obj_t* ov = static_cast<lv_obj_t*>(lv_event_get_target(e));
    for (CollCtx* c : s_colls)
        if (c->candOverlay == ov) { c->candOverlay = nullptr; c->candBox = nullptr; return; }
}

void candPopupClose(CollCtx* c) {
    if (!c->candOverlay) return;
    candScanStop(c->d);
    lv_obj_t* ov = c->candOverlay;
    c->candOverlay = nullptr;     /* the delete callback would do this too, but
                                   * the dismiss is async and a second press
                                   * must not open a second popup meanwhile */
    c->candBox = nullptr;
    dismiss(ov);
}

void onRefreshButton(lv_event_t* e) {
    auto* c = static_cast<CollCtx*>(lv_event_get_user_data(e));
    if (c->candOverlay) return;
    const lcd_candidates_t* cand = c->d->candidates;

    /* Titled for what it is showing, not for the button that opened it: by the
     * time it is on screen the asking is done. */
    lv_obj_t* card = makeModal(&c->candOverlay,
                               (cand->found && *cand->found) ? cand->found
                                                             : cand->refresh_label,
                               [](lv_event_t* ev) {
                                   candPopupClose(static_cast<CollCtx*>(lv_event_get_user_data(ev)));
                               }, c);
    lv_obj_add_event_cb(c->candOverlay, onCandPopupDelete, LV_EVENT_DELETE, nullptr);

    /* The list IS the popup's body: one scroll container, so a busy band scrolls
     * inside the card instead of growing it off the screen. */
    c->candBox = lv_obj_create(card);
    lv_obj_remove_style_all(c->candBox);
    lv_obj_set_width(c->candBox, lv_pct(100));
    lv_obj_set_height(c->candBox, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(c->candBox, (lcdScreenH() * 2) / 3, 0);
    lv_obj_set_flex_flow(c->candBox, LV_FLEX_FLOW_COLUMN);

    candRebuild(c);                       /* whatever the last scan left cached */
    lcdSettingRunAction(cand->refresh);   /* then ask for a fresh one */
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
        /* Leaving the pane stops the scan and takes its popup with it — the
         * popup lives on lv_layer_top, outside the pane's widget tree, so
         * nothing else would. */
        candPopupClose(c);
        candScanStop(c->d);
        delete c;
        return;
    }
}

}  // namespace

/* ================= public ================= */

void lcdSettingsRebootNotice(void) { rebootNotice(); }

void lcdSettingsNotice(const char* text) { notice(text); }

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
    lcdModalCloseAllNow();
}

lv_obj_t* lcdSettingAction(lv_obj_t* parent, const char* label, const lcd_action_t* act,
                           const char* color) {
    /* In the CONTROL column, not across the pane: a button is a control, and a
     * control that starts at the pane's left edge while every field on the
     * pane starts a third of the way in reads as belonging to none of them. */
    lv_obj_t* row = lcdSettingsMakeRow(parent);
    lcdSettingsRowLabel(row, "");
    lv_obj_t* b = lv_button_create(row);
    lcdSettingsFillControl(b);
    lcdSettingsHalfPadVer(b);
    buttonColor(b, color);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, [](lv_event_t* e) {
        lcdSettingRunAction(static_cast<const lcd_action_t*>(lv_event_get_user_data(e)));
    }, LV_EVENT_CLICKED, (void*)act);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return row;
}

lv_obj_t* lcdSettingActionRow(lv_obj_t* parent, lcd_align_t align,
                              const lcd_btn_t* btns, int nbtns) {
    if (!btns || nbtns <= 0) return nullptr;
    lv_obj_t* row = lcdSettingsMakeRow(parent);
    /* An empty label column, so a line of buttons begins where the controls
     * above and below it begin. `align` then says which edge of THAT column
     * they gather at. The row wraps and grows instead of keeping the fixed
     * height an ordinary row has — a pair whose labels are too wide for the
     * display stacks rather than running off the edge of it, which is the
     * difference between a tight pane and a broken one. */
    lcdSettingsRowLabel(row, "");
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 3, 0);

    /* The buttons wrap inside the control column rather than inside the row:
     * `align` has to mean "which edge of the column", and a row that wrapped as
     * a whole would carry the empty label column along with them. */
    lv_flex_align_t main = align == LCD_ALIGN_RIGHT  ? LV_FLEX_ALIGN_END
                         : align == LCD_ALIGN_CENTER ? LV_FLEX_ALIGN_CENTER
                                                     : LV_FLEX_ALIGN_START;
    lv_obj_t* grp = lv_obj_create(row);
    lv_obj_remove_style_all(grp);
    lv_obj_set_height(grp, LV_SIZE_CONTENT);
    lcdSettingsFillControl(grp);
    lv_obj_set_style_pad_column(grp, 6, 0);
    lv_obj_set_style_pad_row(grp, 4, 0);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grp, main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < nbtns; i++) {
        lv_obj_t* b = lv_button_create(grp);
        lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);   /* share the line */
        lv_obj_set_style_pad_hor(b, 12, 0);
        lv_obj_set_style_pad_ver(b, 2, 0);
        buttonColor(b, btns[i].color);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, btns[i].label);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            lcdSettingRunAction(static_cast<const lcd_action_t*>(lv_event_get_user_data(e)));
        }, LV_EVENT_CLICKED, (void*)btns[i].act);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
        /* Gating one button, not the row: a hidden flex child leaves the layout
         * entirely, so the rest close up around it. */
        if (btns[i].when_key && *btns[i].when_key)
            lcdSettingWhenKey(b, btns[i].when_key);
    }
    return row;
}

lv_obj_t* lcdSettingCollection(lv_obj_t* parent, const lcd_collection_t* d) {
    if (!d) return nullptr;
    if (d->label && *d->label)     lcdSettingSection(parent, d->label);
    /* Above the rows, under the heading: what the list is, and what its order
     * means where the order is the operator's to set. The rows themselves are
     * the device's data and have no room to say it. */
    if (d->caption && *d->caption) lcdSettingCaption(parent, d->caption);

    CollCtx* c = new CollCtx();
    c->d      = d;
    c->setCmd = std::string(d->cmd) + ".set";
    c->errKey = std::string(d->cmd) + ".error";
    c->ackKey = std::string(d->cmd) + ".done";
    /* The title is the ITEM's, filled in when the page is opened: the
     * collection's own name over it ("Known networks") says nothing about
     * which one of them this is. It comes from `item:` — the template that
     * already says how an item is titled — so a collection names its detail
     * page by having named its rows. */
    c->editForm = lcd_form_t{ .fields = d->edit, .nfields = d->nedit,
                              .cmd = c->setCmd.c_str(), .submit = "Save",
                              .title = nullptr };

    c->box = lv_obj_create(parent);
    lv_obj_remove_style_all(c->box);
    lv_obj_set_width(c->box, lv_pct(100));
    lv_obj_set_height(c->box, LV_SIZE_CONTENT);
    /* Above and below the banded block, not between the rows: the bands have to
     * meet for the alternation to read as one list. */
    lv_obj_set_style_pad_ver(c->box, 4, 0);
    lv_obj_set_flex_flow(c->box, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c->box, LV_OBJ_FLAG_SCROLLABLE);

    /* The collection's own buttons share one bar under the list, gathered right
     * like a dialog's: scanning first, because finding a thing is what you reach
     * for before describing one by hand. Each is sized to its label, so two of
     * them cost one line instead of two full-width ones. */
    const lcd_candidates_t* cand = d->candidates;
    bool wantScan = cand && cand->refresh && cand->refresh_label && *cand->refresh_label;
    if (wantScan || d->nadds > 0) {
        lv_obj_t* bar = buttonBar(parent);
        if (wantScan)
            barButton(bar, cand->refresh_label, nullptr, onRefreshButton, c);
        for (int i = 0; i < d->nadds; i++) {
            auto* a = new AddBtnCtx{ c, i };
            lv_obj_t* b = barButton(bar, d->adds[i].label, nullptr, onAddButton, a);
            lv_obj_add_event_cb(b, addBtnFree, LV_EVENT_DELETE, a);
        }
    }

    /* Candidates have no place on the pane: they live in the scan popup, which
     * the button above opens. The subscription is the pane's, though, so a scan
     * already running when the popup is reopened fills it at once. */
    if (cand) c->candScope = cand->key;

    c->arrayScope = d->key;
    if (d->status && *d->status) c->statusScope = literalPrefix(d->status);

    s_colls.push_back(c);
    subAdd(c->arrayScope);
    if (!c->candScope.empty())   subAdd(c->candScope);
    if (!c->statusScope.empty()) subAdd(c->statusScope);
    lv_obj_add_event_cb(c->box, collDelete, LV_EVENT_DELETE, nullptr);

    collRebuild(c);
    return c->box;
}
