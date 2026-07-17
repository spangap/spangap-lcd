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
#include "lcd_app.h"
#include "mem.h"

#include "storage.h"
#include "log.h"
#include "compat.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <algorithm>

namespace {

/* ---- registry ---- */

struct Node {
    std::string id;
    std::string label;
    int         placement = 0;         /* sibling ordering pref (see lcdRegisterSettings) */
    lcd_fn_t    fn = nullptr;          /* non-null => leaf item */
    std::vector<Node*> kids;
    Node* find(const std::string& cid) {
        for (auto* k : kids) if (k->id == cid) return k;
        return nullptr;
    }
};
Node s_root;

void titleCase(std::string& s) { if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]); }

/* Bucket placements: positive toward the top, 0 middle, negative toward the bottom. */
int placeRank(int p) { return p > 0 ? 0 : (p < 0 ? 2 : 1); }

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
lv_obj_t* s_pillUp   = nullptr;        /* "more above" hint on the top page */
lv_obj_t* s_pillDn   = nullptr;        /* "more below" hint on the top page */

struct Page { lv_obj_t* obj; Node* node; };
std::vector<Page> s_pages;             /* nav stack; back() = visible top page */

void onRowClick(lv_event_t* e);

/* Refresh the pill visibility from the top page's scroll bounds. Called on every
 * scroll event from any page, and after each push/pop. Non-scrollable pages
 * report scroll_top/bottom == 0, so this is a no-op for them. */
void scrollIndicatorsUpdate() {
    if (s_pages.empty() || !s_pillUp || !s_pillDn) return;
    lv_obj_t* page = s_pages.back().obj;
    bool up = lv_obj_get_scroll_top(page)    > 0;
    bool dn = lv_obj_get_scroll_bottom(page) > 0;
    if (up) lv_obj_remove_flag(s_pillUp, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag   (s_pillUp, LV_OBJ_FLAG_HIDDEN);
    if (dn) lv_obj_remove_flag(s_pillDn, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag   (s_pillDn, LV_OBJ_FLAG_HIDDEN);
}

void onAnyPageScroll(lv_event_t*) { scrollIndicatorsUpdate(); }

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

/* Opaque, full-size page in the host — stacking hides the parent. All pages
 * scroll: item panes can be long, and a menu can outgrow the viewport too
 * (e.g. Settings/Net). LVGL's scroll-vs-tap threshold keeps row clicks working. */
lv_obj_t* makePage() {
    lv_obj_t* pg = lv_obj_create(s_host);
    lv_obj_remove_style_all(pg);
    lv_obj_set_pos(pg, 0, 0);
    lv_obj_set_size(pg, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(pg, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(pg, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(pg, 6, 0);
    lv_obj_set_style_pad_row(pg, 6, 0);
    lv_obj_add_event_cb(pg, onAnyPageScroll, LV_EVENT_SCROLL, nullptr);
    return pg;
}

/* Called once the new top page is on the stack AND its content is built, so
 * the layout pass sees the final content height before we read scroll bounds.
 * Also re-floats the pills on top of the freshly-pushed page. */
void afterPagePush(lv_obj_t* pg) {
    if (s_pillUp) lv_obj_move_foreground(s_pillUp);
    if (s_pillDn) lv_obj_move_foreground(s_pillDn);
    lv_obj_update_layout(pg);
    scrollIndicatorsUpdate();
}

void pushMenu(Node* menu) {
    dbg("settings pushMenu '%s' kids=%d\n",                 /* TEMP diag */
        menu == &s_root ? "root" : menu->label.c_str(), (int)menu->kids.size());
    lv_obj_t* pg = makePage();
    /* Order children by placement, then alphabetically (case-insensitive) within
     * an equal preference. Registration order is boot/dependency order, which
     * isn't meaningful to the user; sort a copy so the tree itself is left
     * untouched. Mirrors the web menu's placement sort. */
    std::vector<Node*> kids = menu->kids;
    std::sort(kids.begin(), kids.end(), [](const Node* a, const Node* b) {
        int ra = placeRank(a->placement), rb = placeRank(b->placement);
        if (ra != rb) return ra < rb;
        if (a->placement != b->placement) return a->placement < b->placement;
        std::string la = a->label, lb = b->label;
        for (char& c : la) c = (char)tolower((unsigned char)c);
        for (char& c : lb) c = (char)tolower((unsigned char)c);
        return la < lb;
    });
    for (Node* k : kids) {
        lv_obj_t* row = lv_button_create(pg);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 38);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x20262e), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_add_event_cb(row, onRowClick, LV_EVENT_CLICKED, k);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {    /* TEMP diag */
            dbg("settings row PRESSED: %s\n",
                static_cast<Node*>(lv_event_get_user_data(e))->label.c_str());
        }, LV_EVENT_PRESSED, k);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, k->label.c_str());
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        /* Menu items get a larger face than the in-pane controls (which keep the
         * inherited size) — these are the primary tap targets. Scaled by zoom. */
        lv_obj_set_style_text_font(lbl, lcdFont(LcdFace::UI, lcdPx(16)), 0);
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
    afterPagePush(pg);
}

void pushItem(Node* item) {
    lv_obj_t* pg = makePage();
    s_pages.push_back({ pg, item });
    updateHeader();
    item->fn(pg);                      /* build the pane into this page */
    afterPagePush(pg);
}

void popPage() {
    if (s_pages.size() <= 1) { lcdGoHomeInternal(); return; }  /* at root: exit */
    lv_obj_t* top = s_pages.back().obj;
    s_pages.pop_back();
    lv_obj_delete(top);                /* reveal the parent beneath, scroll intact */
    updateHeader();
    scrollIndicatorsUpdate();
}

void onRowClick(lv_event_t* e) {
    Node* n = static_cast<Node*>(lv_event_get_user_data(e));
    if (!n) return;
    dbg("settings row CLICKED: %s\n", n->label.c_str());   /* TEMP diag */
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
    lv_obj_set_style_text_font(s_titleLbl, lcdFont(LcdFace::UI_BOLD, 16), 0);  /* a touch bigger */
    lv_obj_align(s_titleLbl, LV_ALIGN_LEFT_MID, 34, 0);

    s_host = lv_obj_create(layer);
    lv_obj_remove_style_all(s_host);
    lv_obj_set_pos(s_host, 0, SETTINGS_HDR_H);
    lv_obj_set_size(s_host, lv_pct(100), layerH - SETTINGS_HDR_H);
    lv_obj_remove_flag(s_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_host, LV_OBJ_FLAG_CLICKABLE);

    /* Scroll-overflow pills: small rounded chips at the right edge of the host,
     * shown when the top page has content above (↑) or below (↓). Persistent
     * across navigation; afterPagePush() moves them above each freshly-pushed
     * page so they always float on top. */
    auto makePill = [](const char* sym, lv_align_t align) {
        lv_obj_t* p = lv_obj_create(s_host);
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, 24, 16);
        lv_obj_set_style_bg_color(p, lv_color_hex(0x3a4658), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
        lv_obj_set_style_radius(p, 8, 0);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag   (p, LV_OBJ_FLAG_HIDDEN);
        int y = (align == LV_ALIGN_TOP_RIGHT) ? 4 : -4;
        lv_obj_align(p, align, -4, y);
        lv_obj_t* l = lv_label_create(p);
        lv_label_set_text(l, sym);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);
        return p;
    };
    s_pillUp = makePill(LV_SYMBOL_UP,   LV_ALIGN_TOP_RIGHT);
    s_pillDn = makePill(LV_SYMBOL_DOWN, LV_ALIGN_BOTTOM_RIGHT);

    s_pages.clear();
    pushMenu(&s_root);                 /* root page */
}

/* ---- row scaffolding ---- */

/* Two-column row: a 1/3 label (right-aligned, hugging the divider) and a 2/3
 * control area (left-aligned, so the control sits right next to its label rather
 * than pushed to the far edge). Helpers add the control after addRowLabel(); the
 * fillRowControl() helper stretches a control across the 2/3 where that reads
 * better (dropdown / value / slider group). */
lv_obj_t* makeRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 36);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    /* Rows must not scroll themselves — a vertical drag starting on a row should
     * chain to the page, not bounce the row's own content out of view. */
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void addRowLabel(lv_obj_t* row, const char* text) {
    lv_obj_t* l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_width(l, lv_pct(33));                  /* 1/3 label column */
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
}

/* Stretch a control across the remaining 2/3, left-aligned. */
void fillRowControl(lv_obj_t* w) { lv_obj_set_flex_grow(w, 1); }

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
/* Live numeric readout beside a slider (user_data = the value label). */
void onSliderNum(lv_event_t* e) {
    lv_obj_t* num = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_obj_t* s   = static_cast<lv_obj_t*>(lv_event_get_target_obj(e));
    lv_label_set_text_fmt(num, "%d", (int)lv_slider_get_value(s));
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

void lcdRegisterSettings(const char* path, const char* label, lcd_fn_t fn, int placement) {
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
        if (i + 1 == segs.size()) { n->label = label ? label : n->label.c_str(); n->fn = fn; n->placement = placement; }
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

lv_obj_t* lcdSettingCaption(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);    /* wrap within the pane width */
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_color(l, lv_color_hex(0x8a93a0), 0);
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

    /* Control group: the slider + a live numeric readout, left-aligned in the
     * 2/3 column (a slider alone hides the exact value). */
    lv_obj_t* grp = lv_obj_create(row);
    lv_obj_remove_style_all(grp);
    lv_obj_set_height(grp, LV_SIZE_CONTENT);
    fillRowControl(grp);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grp, 8, 0);
    lv_obj_remove_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* num = lv_label_create(grp);
    lv_obj_set_style_text_color(num, lv_color_hex(0xb0b8c0), 0);
    lv_label_set_text_fmt(num, "%d", storageGetInt(key, min));

    lv_obj_t* s = lv_slider_create(grp);
    lv_obj_set_width(s, 120);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, storageGetInt(key, min), LV_ANIM_OFF);
    lv_obj_add_event_cb(s, onSlider,    LV_EVENT_VALUE_CHANGED, (void*)key);
    lv_obj_add_event_cb(s, onSliderNum, LV_EVENT_VALUE_CHANGED, num);   /* immediate readout */
    bindAttach(s,   key, BK_SLIDER);
    bindAttach(num, key, BK_VALUE);   /* external writes refresh the number too */
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
        fillRowControl(ta);                               /* 2/3 column */
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), ta);
        lv_obj_add_event_cb(ta, onInlineCommit, LV_EVENT_READY,     (void*)key);
        lv_obj_add_event_cb(ta, onInlineCommit, LV_EVENT_DEFOCUSED, (void*)key);
        bindAttach(ta, key, BK_TEXTAREA, secret);
        return row;
    }

    /* No hardware keyboard: value label + full-screen on-screen-keyboard editor. */
    lv_obj_t* val = lv_label_create(row);
    lv_obj_set_style_text_color(val, lv_color_hex(0xb0b8c0), 0);
    fillRowControl(val);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_LEFT, 0);
    setValueText(val, storageGetStr(key, ""), secret);

    auto* tr = static_cast<TextRef*>(gp_alloc(sizeof(TextRef)));
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
    fillRowControl(d);                                    /* 2/3 column */
    std::string opts(optionsCsv ? optionsCsv : "");
    for (auto& c : opts) if (c == ',') c = '\n';
    lv_dropdown_set_options(d, opts.c_str());
    dropdownSelect(d, storageGetStr(key, "").c_str());
    lv_obj_add_event_cb(d, onDropdown, LV_EVENT_VALUE_CHANGED, (void*)key);
    bindAttach(d, key, BK_DROPDOWN);
    return row;
}

#if CONFIG_LCD_SETTINGS_MARQUEE
/* Focus-driven marquee (Brookesia-watch style): only the focused row scrolls its
 * full value, the rest stay ellipsized — a panel of hashes all marqueeing at once
 * would be noise. Flips long-mode on the keypad focus ring's FOCUSED/DEFOCUSED. */
static void marqueeFocusCb(lv_event_t* e) {
    lv_obj_t* lbl = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED)
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    else
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
}

/* Make a read-only value label single-line, ellipsized, keypad-focusable, and
 * marquee-on-focus. flex_grow bounds its width to the row's free space (a
 * content-sized label can't scroll — width must be < the text). */
void valueLabelMarquee(lv_obj_t* lbl) {
    lv_obj_set_flex_grow(lbl, 1);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);   /* 2/3 column, left-aligned */
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), lbl);
    lv_obj_add_event_cb(lbl, marqueeFocusCb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(lbl, marqueeFocusCb, LV_EVENT_DEFOCUSED, nullptr);
}
#endif

lv_obj_t* lcdSettingValue(lv_obj_t* parent, const char* label, const char* key) {
    std::string v = storageGetStr(key, "");

#if !CONFIG_LCD_SETTINGS_MARQUEE
    /* Long values (identity / dest hashes, paths) overrun the shared single-line
     * row and collide with the label — stack the label over a wrapped value.
     * (With the marquee tunable on, the focus-driven horizontal scroll below
     * replaces this vertical stack.) */
    if (v.size() > 18) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_pad_row(row, 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lab = lv_label_create(row);
        lv_label_set_text(lab, label);
        lv_obj_set_style_text_color(lab, lv_color_hex(0x8a93a0), 0);

        lv_obj_t* val = lv_label_create(row);
        lv_obj_set_style_text_color(val, lv_color_hex(0xb0b8c0), 0);
        lv_obj_set_width(val, lv_pct(100));
        lv_label_set_long_mode(val, LV_LABEL_LONG_WRAP);
        lv_label_set_text(val, v.c_str());
        bindAttach(val, key, BK_VALUE);
        return row;
    }
#endif

    lv_obj_t* row = makeRow(parent);
    addRowLabel(row, label);
    lv_obj_t* val = lv_label_create(row);
    lv_obj_set_style_text_color(val, lv_color_hex(0xb0b8c0), 0);
    fillRowControl(val);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(val, v.empty() ? "\xE2\x80\x94" : v.c_str());
    bindAttach(val, key, BK_VALUE);   /* event-driven: storage change -> label (no poll) */
#if CONFIG_LCD_SETTINGS_MARQUEE
    valueLabelMarquee(val);
#endif
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

/* SettingsApp — a thin LcdApp host around the existing page-stack. onCreate
 * builds the menu tree (settingsOpen) into the app's root; the slash-path
 * registry, the lcdSetting* builders, two-way storage binding, scroll pills and
 * every straddle's pane hook are byte-for-byte the code that shipped before. */
namespace {
class SettingsApp : public LcdApp {
public:
    SettingsApp() : LcdApp({ .name = "Settings", .iconBasename = "gear", .launcherPage = 0 }) {}
    void onCreate(lv_obj_t* root) override { settingsOpen(root); }
    void onClose() override { s_pages.clear(); }   /* drop dangling page pointers */
};
}  // namespace

/* ---- built-in Display / UI-zoom stepper (plan §6) ---- */
namespace {
lv_obj_t* s_zoomLbl = nullptr;

void zoomAdjust(int delta) {
    int v = storageGetInt("s.lcd.scale", 100) + delta;
    if (v < 50)  v = 50;
    if (v > 200) v = 200;
    storageSet("s.lcd.scale", v);      /* → shellApplyZoom() via the lcd.cpp sub */
    if (s_zoomLbl) lv_label_set_text_fmt(s_zoomLbl, "%d%%", v);
}

/* A −/+ stepper (25% steps, clamped 50–200) bound to s.lcd.scale. Writing the
 * key reflows the whole shell (fonts, launcher grid, icons) live. */
void zoomPane(void* arg) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(arg);
    lcdSettingCaption(parent,
        "Scale the whole interface (50–200%). Content reflows — text and icons "
        "stay crisp at every step, not magnified.");

    lv_obj_t* row = makeRow(parent);
    addRowLabel(row, "UI Zoom");

    lv_obj_t* grp = lv_obj_create(row);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grp, 12, 0);
    lv_obj_remove_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    auto stepBtn = [](lv_obj_t* p, const char* sym, lv_event_cb_t cb) {
        lv_obj_t* b = lv_button_create(p);
        lv_obj_set_size(b, 30, 30);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, sym);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
        return b;
    };
    stepBtn(grp, LV_SYMBOL_MINUS, [](lv_event_t*) { zoomAdjust(-25); });

    s_zoomLbl = lv_label_create(grp);
    lv_obj_set_style_text_color(s_zoomLbl, lv_color_white(), 0);
    lv_label_set_text_fmt(s_zoomLbl, "%d%%", storageGetInt("s.lcd.scale", 100));

    stepBtn(grp, LV_SYMBOL_PLUS, [](lv_event_t*) { zoomAdjust(25); });
}
}  // namespace

void lcdSettingsInit(void) {
    s_root.label = "Settings";
    lcdRegisterSettings("display/zoom", "UI Zoom", zoomPane, 1);
    lcdInstall(new SettingsApp());
}
