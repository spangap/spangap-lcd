/**
 * lcd_settings.cpp — the built-in Settings program (gear) + its menu, plus the
 * lcdSetting* helpers that build uniform storage-bound rows (the on-device
 * analogue of the browser's Setting* components).
 *
 * lcdRegisterSettings() populates an in-RAM menu tree (works from any init task,
 * even before lcdInit). The gear program builds the UI lazily on the lcd task:
 * a shared header (title + back) over a stack of pages. Each submenu / item
 * pane is its own opaque page; descending pushes a new one on top, Back deletes
 * it to reveal the parent (scroll position preserved). See the nav block below.
 *
 * NOTE: storage keys passed to the helpers must be string literals / static —
 * they're stored by pointer (pages are created and destroyed as you navigate,
 * so we deliberately don't strdup).
 */
#include "lcd_internal.h"

#include "storage.h"
#include "log.h"
#include "compat.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cctype>

namespace {

/* ---- registry ---- */

struct Node {
    std::string id;
    std::string label;
    lcd_fn_t    fn = nullptr;          /* non-null => leaf item */
    std::vector<Node*> kids;
    Node* find(const std::string& cid) {
        for (auto* k : kids) if (k->id == cid) return k;
        return nullptr;
    }
};
Node s_root;

void titleCase(std::string& s) { if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]); }

/* ---- nav UI state (lcd task only) ----
 * Each menu level and each item pane is its own opaque, full-size page stacked
 * in s_host. Descending pushes a new page ON TOP — the parent stays alive,
 * untouched, beneath it; Back deletes the top page, revealing the parent
 * exactly as it was, scroll position included. The header (back + title) lives
 * outside the pages, so Back never deletes the widget whose event it's handling
 * and descending never deletes the row being clicked (the old rebuild-in-place
 * scheme cleaned the content out from under the live click event). */

const int SETTINGS_HDR_H = 30;

lv_obj_t* s_titleLbl = nullptr;
lv_obj_t* s_back     = nullptr;
lv_obj_t* s_host     = nullptr;        /* holds the page stack, below the header */

struct Page { lv_obj_t* obj; Node* node; };
std::vector<Page> s_pages;             /* nav stack; back() = visible top page */

void onRowClick(lv_event_t* e);

void updateHeader() {
    if (s_pages.empty()) return;
    /* Breadcrumb ("Settings/Net/Wifi") so you can see where you are. */
    std::string path;
    for (size_t i = 0; i < s_pages.size(); i++) {
        if (i) path += "/";
        path += s_pages[i].node->label;
    }
    lv_label_set_text(s_titleLbl, path.c_str());
    if (s_pages.size() <= 1) lv_obj_add_flag   (s_back, LV_OBJ_FLAG_HIDDEN);
    else                     lv_obj_remove_flag(s_back, LV_OBJ_FLAG_HIDDEN);
}

/* Opaque, full-size page in the host — stacking hides the parent. Menu pages
 * are non-scrollable (they're short, and a scrollable parent can swallow a row
 * tap as a scroll); item panes scroll since they can be long. */
lv_obj_t* makePage(bool scrollable) {
    lv_obj_t* pg = lv_obj_create(s_host);
    lv_obj_remove_style_all(pg);
    lv_obj_set_pos(pg, 0, 0);
    lv_obj_set_size(pg, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(pg, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(pg, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(pg, 6, 0);
    lv_obj_set_style_pad_row(pg, 6, 0);
    if (!scrollable) lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return pg;
}

void pushMenu(Node* menu) {
    info("settings pushMenu '%s' kids=%d\n",                 /* TEMP diag */
         menu == &s_root ? "root" : menu->label.c_str(), (int)menu->kids.size());
    lv_obj_t* pg = makePage(false);
    for (Node* k : menu->kids) {
        lv_obj_t* row = lv_button_create(pg);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 38);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x20262e), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_add_event_cb(row, onRowClick, LV_EVENT_CLICKED, k);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {    /* TEMP diag */
            info("settings row PRESSED: %s\n",
                 static_cast<Node*>(lv_event_get_user_data(e))->label.c_str());
        }, LV_EVENT_PRESSED, k);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, k->label.c_str());
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

        if (!k->fn) {                  /* submenu chevron */
            lv_obj_t* ch = lv_label_create(row);
            lv_label_set_text(ch, LV_SYMBOL_RIGHT);
            lv_obj_set_style_text_color(ch, lv_color_hex(0x8a93a0), 0);
            lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -10, 0);
        }
    }
    s_pages.push_back({ pg, menu });
    updateHeader();
}

void pushItem(Node* item) {
    lv_obj_t* pg = makePage(true);
    s_pages.push_back({ pg, item });
    updateHeader();
    item->fn(pg);                      /* build the pane into this page */
}

void popPage() {
    if (s_pages.size() <= 1) { lcdGoHomeInternal(); return; }  /* at root: exit */
    lv_obj_t* top = s_pages.back().obj;
    s_pages.pop_back();
    lv_obj_delete(top);                /* reveal the parent beneath, scroll intact */
    updateHeader();
}

void onRowClick(lv_event_t* e) {
    Node* n = static_cast<Node*>(lv_event_get_user_data(e));
    if (!n) return;
    info("settings row CLICKED: %s\n", n->label.c_str());   /* TEMP diag */
    if (n->fn) pushItem(n);            /* item -> pane page */
    else       pushMenu(n);            /* submenu -> descend */
}

void settingsOpen(void* arg) {
    lv_obj_t* layer = static_cast<lv_obj_t*>(arg);
    int layerH = lcdScreenH() - LCD_STATUSBAR_H;

    lv_obj_t* hdr = lv_obj_create(layer);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, lv_pct(100), SETTINGS_HDR_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x222b38), 0);   /* distinct from the page bg */
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(0x3a4658), 0);
    lv_obj_set_style_border_width(hdr, 1, 0);

    s_back = lv_label_create(hdr);
    lv_label_set_text(s_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(s_back, lv_color_white(), 0);
    lv_obj_align(s_back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_back, 12);
    lv_obj_add_event_cb(s_back, [](lv_event_t*) { popPage(); }, LV_EVENT_CLICKED, nullptr);

    s_titleLbl = lv_label_create(hdr);
    lv_obj_set_style_text_color(s_titleLbl, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(s_titleLbl, &lv_font_montserrat_16, 0);   /* a touch bigger */
#endif
    lv_obj_align(s_titleLbl, LV_ALIGN_LEFT_MID, 34, 0);

    s_host = lv_obj_create(layer);
    lv_obj_remove_style_all(s_host);
    lv_obj_set_pos(s_host, 0, SETTINGS_HDR_H);
    lv_obj_set_size(s_host, lv_pct(100), layerH - SETTINGS_HDR_H);

    s_pages.clear();
    pushMenu(&s_root);                 /* root page */
}

/* ---- row scaffolding ---- */

lv_obj_t* makeRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 36);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    return row;
}

void addRowLabel(lv_obj_t* row, const char* text) {
    lv_obj_t* l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
}

/* ---- two-way storage binding ----
 * Every storage-bound control registers here: its change handler writes the key,
 * and we subscribe the key so an external write (browser, CLI, another task)
 * flows back into the control. Storage change callbacks are dispatched on the
 * lcd task, so bindDispatch touches LVGL directly. Bindings tear down on widget
 * delete (page nav), and a key is unsubscribed once its last binding goes — so
 * nothing leaks across navigation. lcd-task-only; no locks. */
void setValueText(lv_obj_t* lbl, const std::string& v, bool secret);   /* fwd */
void dropdownSelect(lv_obj_t* d, const char* val);                     /* fwd */

enum BindKind { BK_SWITCH, BK_SLIDER, BK_DROPDOWN, BK_TEXTLBL, BK_TEXTAREA, BK_VALUE };
struct Bind { std::string key; lv_obj_t* w; BindKind kind; bool secret; };
std::vector<Bind> s_binds;

void bindApply(const Bind& b, const char* val) {
    switch (b.kind) {
        case BK_SWITCH:
            if (atoi(val)) lv_obj_add_state(b.w, LV_STATE_CHECKED);
            else           lv_obj_remove_state(b.w, LV_STATE_CHECKED);
            break;
        case BK_SLIDER:   lv_slider_set_value(b.w, atoi(val), LV_ANIM_OFF); break;
        case BK_DROPDOWN: dropdownSelect(b.w, val); break;
        case BK_TEXTLBL:  setValueText(b.w, val ? val : "", b.secret); break;
        case BK_TEXTAREA: /* don't clobber the field while it's being edited */
            if (!(lv_obj_get_state(b.w) & LV_STATE_FOCUSED))
                lv_textarea_set_text(b.w, val ? val : "");
            break;
        case BK_VALUE:    lv_label_set_text(b.w, (val && *val) ? val : "\xE2\x80\x94"); break;
    }
}

void bindDispatch(const char* key, const char* val) {     /* storage cb; lcd task */
    for (auto& b : s_binds) if (b.key == key) bindApply(b, val);
}

void bindDelete(lv_event_t* e) {
    lv_obj_t* w = static_cast<lv_obj_t*>(lv_event_get_target(e));
    std::string key;
    for (auto it = s_binds.begin(); it != s_binds.end(); )
        if (it->w == w) { key = it->key; it = s_binds.erase(it); } else ++it;
    if (key.empty()) return;
    for (auto& b : s_binds) if (b.key == key) return;     /* others still need it */
    storageUnsubscribe(key.c_str());
}

void bindAttach(lv_obj_t* w, const char* key, BindKind kind, bool secret = false) {
    bool have = false;
    for (auto& b : s_binds) if (b.key == key) { have = true; break; }
    s_binds.push_back({ key, w, kind, secret });
    if (!have) storageSubscribeChanges(key, bindDispatch);
    lv_obj_add_event_cb(w, bindDelete, LV_EVENT_DELETE, nullptr);
}

/* ---- text editor (on-screen keyboard) ---- */

struct TextRef { char key[64]; bool secret; lv_obj_t* valLbl; };
struct { char key[64]; bool secret; lv_obj_t* valLbl; lv_obj_t* overlay; lv_obj_t* ta; } s_ed;

void setValueText(lv_obj_t* lbl, const std::string& v, bool secret) {
    if (v.empty())     lv_label_set_text(lbl, "\xE2\x80\x94");
    else if (secret)   lv_label_set_text(lbl, "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
    else               lv_label_set_text(lbl, v.c_str());
}

void editorClose(bool commit) {
    if (commit) {
        std::string v = lv_textarea_get_text(s_ed.ta);
        storageSet(s_ed.key, v.c_str());
        if (s_ed.valLbl) setValueText(s_ed.valLbl, v, s_ed.secret);
    }
    if (s_ed.overlay) { lv_obj_delete(s_ed.overlay); s_ed.overlay = nullptr; }
}
void kbEvent(lv_event_t* e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY)       editorClose(true);
    else if (c == LV_EVENT_CANCEL) editorClose(false);
}
void onTextRow(lv_event_t* e) {
    auto* tr = static_cast<TextRef*>(lv_event_get_user_data(e));
    safeStrncpy(s_ed.key, tr->key, sizeof(s_ed.key));
    s_ed.secret = tr->secret;
    s_ed.valLbl = tr->valLbl;

    lv_obj_t* ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    s_ed.overlay = ov;

    lv_obj_t* ta = lv_textarea_create(ov);
    lv_obj_set_size(ta, lv_pct(96), 56);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 6);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, tr->secret);
    lv_textarea_set_text(ta, storageGetStr(tr->key, "").c_str());
    s_ed.ta = ta;

    if (lcdHasKeyboard()) {
        /* Physical keyboard present: no on-screen keyboard. Type straight into
         * the textarea (the keyboard indev delivers keys to the focused object);
         * ENTER fires LV_EVENT_READY -> commit + close. */
        lv_group_add_obj(lcdInputGroup(), ta);
        lv_group_focus_obj(ta);
        lv_obj_add_event_cb(ta, kbEvent, LV_EVENT_READY, nullptr);
    } else {
        lv_obj_t* kb = lv_keyboard_create(ov);
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_add_event_cb(kb, kbEvent, LV_EVENT_READY,  nullptr);
        lv_obj_add_event_cb(kb, kbEvent, LV_EVENT_CANCEL, nullptr);
    }
}
void textRefDelete(lv_event_t* e) { free(lv_event_get_user_data(e)); }

/* ---- control event handlers ---- */

void onSwitch(lv_event_t* e) {
    const char* key = static_cast<const char*>(lv_event_get_user_data(e));
    lv_obj_t* sw = static_cast<lv_obj_t*>(lv_event_get_target_obj(e));
    storageSet(key, lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0);
}
void onSlider(lv_event_t* e) {
    const char* key = static_cast<const char*>(lv_event_get_user_data(e));
    lv_obj_t* s = static_cast<lv_obj_t*>(lv_event_get_target_obj(e));
    storageSet(key, (int)lv_slider_get_value(s));
}
void onDropdown(lv_event_t* e) {
    const char* key = static_cast<const char*>(lv_event_get_user_data(e));
    lv_obj_t* d = static_cast<lv_obj_t*>(lv_event_get_target_obj(e));
    char buf[64];
    lv_dropdown_get_selected_str(d, buf, sizeof(buf));
    storageSet(key, buf);
}
void onButton(lv_event_t* e) {
    auto fn = reinterpret_cast<lcd_fn_t>(lv_event_get_user_data(e));
    if (fn) fn(lv_event_get_target_obj(e));
}

/* Select the dropdown option whose text equals val (used at build + on bind). */
void dropdownSelect(lv_obj_t* d, const char* val) {
    if (!val || !*val) return;
    std::string all = lv_dropdown_get_options(d);
    all += "\n";
    uint32_t idx = 0;
    std::string seg;
    for (char c : all) {
        if (c == '\n') { if (seg == val) { lv_dropdown_set_selected(d, idx); return; } seg.clear(); idx++; }
        else seg += c;
    }
}

/* Commit an in-place text field (physical-keyboard path) to storage. */
void onInlineCommit(lv_event_t* e) {
    const char* key = static_cast<const char*>(lv_event_get_user_data(e));
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    storageSet(key, lv_textarea_get_text(ta));
}

}  // namespace

/* ================= public registry ================= */

void lcdRegisterSettings(const char* path, const char* label, lcd_fn_t fn) {
    if (!path || !*path || !fn) return;
    std::vector<std::string> segs;
    std::string s;
    for (const char* p = path; *p; p++) {
        if (*p == '/') { if (!s.empty()) segs.push_back(s); s.clear(); }
        else s += *p;
    }
    if (!s.empty()) segs.push_back(s);
    if (segs.empty()) return;

    Node* cur = &s_root;
    for (size_t i = 0; i < segs.size(); i++) {
        Node* n = cur->find(segs[i]);
        if (!n) {
            n = new Node();
            n->id = segs[i];
            n->label = segs[i];
            titleCase(n->label);
            cur->kids.push_back(n);
        }
        if (i + 1 == segs.size()) { n->label = label ? label : n->label.c_str(); n->fn = fn; }
        cur = n;
    }
}

/* ================= helpers ================= */

lv_obj_t* lcdSettingSection(lv_obj_t* parent, const char* title) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, title);
    lv_obj_set_style_text_color(l, lv_color_hex(0x6cc0ff), 0);
    lv_obj_set_style_pad_top(l, 6, 0);
    return l;
}

lv_obj_t* lcdSettingSwitch(lv_obj_t* parent, const char* label, const char* key) {
    lv_obj_t* row = makeRow(parent);
    addRowLabel(row, label);
    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_set_size(sw, 36, 18);                              /* compact (~60% of default height) */
    /* High off-state contrast: a light knob on a clearly darker track. */
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3a4150), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2563a0),
                              (lv_style_selector_t)LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
    if (storageGetInt(key, 0)) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, onSwitch, LV_EVENT_VALUE_CHANGED, (void*)key);
    bindAttach(sw, key, BK_SWITCH);
    return row;
}

lv_obj_t* lcdSettingSlider(lv_obj_t* parent, const char* label, const char* key,
                           int min, int max) {
    lv_obj_t* row = makeRow(parent);
    addRowLabel(row, label);
    lv_obj_t* s = lv_slider_create(row);
    lv_obj_set_width(s, lv_pct(50));
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, storageGetInt(key, min), LV_ANIM_OFF);
    lv_obj_add_event_cb(s, onSlider, LV_EVENT_VALUE_CHANGED, (void*)key);
    bindAttach(s, key, BK_SLIDER);
    return row;
}

lv_obj_t* lcdSettingText(lv_obj_t* parent, const char* label, const char* key, bool secret) {
    lv_obj_t* row = makeRow(parent);
    addRowLabel(row, label);

    if (lcdHasKeyboard()) {
        /* Physical keyboard: edit in place. The value is an inline one-line
         * textarea; focusing it (tap / trackball) types straight in, Enter or
         * moving away commits. No full-screen on-screen-keyboard pane. */
        lv_obj_t* ta = lv_textarea_create(row);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_password_mode(ta, secret);
        lv_textarea_set_text(ta, storageGetStr(key, "").c_str());
        lv_obj_set_width(ta, lv_pct(55));
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), ta);
        lv_obj_add_event_cb(ta, onInlineCommit, LV_EVENT_READY,     (void*)key);
        lv_obj_add_event_cb(ta, onInlineCommit, LV_EVENT_DEFOCUSED, (void*)key);
        bindAttach(ta, key, BK_TEXTAREA, secret);
        return row;
    }

    /* No hardware keyboard: value label + full-screen on-screen-keyboard editor. */
    lv_obj_t* val = lv_label_create(row);
    lv_obj_set_style_text_color(val, lv_color_hex(0xb0b8c0), 0);
    setValueText(val, storageGetStr(key, ""), secret);

    auto* tr = static_cast<TextRef*>(malloc(sizeof(TextRef)));
    safeStrncpy(tr->key, key, sizeof(tr->key));
    tr->secret = secret;
    tr->valLbl = val;
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, onTextRow, LV_EVENT_CLICKED, tr);
    lv_obj_add_event_cb(row, textRefDelete, LV_EVENT_DELETE, tr);
    bindAttach(val, key, BK_TEXTLBL, secret);
    return row;
}

lv_obj_t* lcdSettingDropdown(lv_obj_t* parent, const char* label, const char* key,
                             const char* optionsCsv) {
    lv_obj_t* row = makeRow(parent);
    addRowLabel(row, label);
    lv_obj_t* d = lv_dropdown_create(row);
    std::string opts(optionsCsv ? optionsCsv : "");
    for (auto& c : opts) if (c == ',') c = '\n';
    lv_dropdown_set_options(d, opts.c_str());
    dropdownSelect(d, storageGetStr(key, "").c_str());
    lv_obj_add_event_cb(d, onDropdown, LV_EVENT_VALUE_CHANGED, (void*)key);
    bindAttach(d, key, BK_DROPDOWN);
    return row;
}

lv_obj_t* lcdSettingValue(lv_obj_t* parent, const char* label, const char* key) {
    lv_obj_t* row = makeRow(parent);
    addRowLabel(row, label);
    lv_obj_t* val = lv_label_create(row);
    lv_obj_set_style_text_color(val, lv_color_hex(0xb0b8c0), 0);
    std::string v = storageGetStr(key, "");
    lv_label_set_text(val, v.empty() ? "\xE2\x80\x94" : v.c_str());
    bindAttach(val, key, BK_VALUE);   /* event-driven: storage change -> label (no poll) */
    return row;
}

lv_obj_t* lcdSettingButton(lv_obj_t* parent, const char* label, lcd_fn_t onClick) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, onButton, LV_EVENT_CLICKED, reinterpret_cast<void*>(onClick));
    return b;
}

/* ================= gear program ================= */

void lcdSettingsInit(void) {
    s_root.label = "Settings";
    lcdLauncherAdd("Settings", "gear", settingsOpen);
}
