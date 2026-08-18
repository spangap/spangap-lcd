/**
 * lcd_settings.cpp — the built-in Settings program (gear) + its tree, plus the
 * lcdSetting* helpers that build uniform storage-bound rows (the on-device
 * analogue of the browser's Setting* components).
 *
 * ONE TREE, no leaf/container split. Every node holds a list of row-block
 * builders AND a list of children; entering it renders the rows first, then a
 * chevron row per child. No node is owned: lcdSettingsContribute() names a path
 * of ids, conjures whatever is missing, and appends its builder — so several
 * straddles contributing at the same path concatenate. A node with no rows and
 * no rendering descendant gets no chevron row: naming a menu is not the same as
 * showing one. It populates an in-RAM registry, so it works from any init task,
 * even before lcdInit.
 *
 * The gear program builds the UI lazily on the lcd task: a shared header (title
 * + back) over a stack of pages. Each node is its own opaque page; descending
 * pushes a new one on top, Back deletes it to reveal the parent (scroll
 * position preserved). See the nav block below.
 *
 * Siblings sort by the one global rule shared with the web surface and the
 * generator: nodes carrying an order first, ascending; everything else after
 * them in contribution order. Contribution order is init order (the generator
 * emits pre-sorted), which is meaningful — the platform's own nodes land before
 * a consumer's.
 *
 * NOTE: storage keys passed to the helpers must be string literals / static —
 * they're stored by pointer (pages are created and destroyed as you navigate,
 * so we deliberately don't strdup).
 */
#include "lcd_internal.h"
#include "lcd_settings_priv.h"
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
    std::string label;                 /* long name, shown in the parent's nav row */
    std::string shortName;             /* header text; defaults to the label */
    bool        named      = false;    /* somebody supplied a label / short / order */
    bool        shortNamed = false;
    int         order    = 0;
    bool        hasOrder = false;
    int         arrival  = 0;          /* contribution order — the unordered tie-break */
    std::vector<lcd_fn_t> builders;    /* the row blocks contributed at this node */
    std::vector<Node*>    kids;
    Node* find(const std::string& cid) {
        for (auto* k : kids) if (k->id == cid) return k;
        return nullptr;
    }
};
Node s_root;
int  s_arrival = 0;

void titleCase(std::string& s) { if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]); }

/* A node renders if it has anything to show: its own rows, or a descendant
 * that has. Declaring a node is therefore not the same as putting it on the
 * screen — a straddle may name a menu and give it an order while it is still
 * empty, and it appears the moment somebody contributes to it. */
bool nodeRenders(const Node* n) {
    if (!n->builders.empty()) return true;
    for (const Node* k : n->kids) if (nodeRenders(k)) return true;
    return false;
}

/* THE sibling order, identical on both surfaces and in the generator: nodes
 * carrying an order first, ascending; everything else after them, in
 * contribution order. No buckets, no alphabetic tier. */
bool nodeLess(const Node* a, const Node* b) {
    if (a->hasOrder != b->hasOrder) return a->hasOrder;
    if (a->hasOrder && a->order != b->order) return a->order < b->order;
    return a->arrival < b->arrival;
}

/* ---- nav UI state (lcd task only) ----
 * Each node is its own opaque, full-size page stacked in s_host. Descending
 * pushes a new page ON TOP — the parent stays alive, untouched, beneath it;
 * Back deletes the top page, revealing the parent exactly as it was, scroll
 * position included. The header (back + title) lives outside the pages, so Back
 * never deletes the widget whose event it's handling and descending never
 * deletes the row being clicked (the old rebuild-in-place scheme cleaned the
 * content out from under the live click event). */

const int SETTINGS_HDR_H = 30;
const int SETTINGS_ROW_H = 36;   /* one control row; the pane's vertical unit */

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
    /* Breadcrumb ("Settings/Net/Wifi") so you can see where you are. Built from
     * the SHORT names: this is a 30px strip on a phone-sized screen, which is
     * exactly why a node may carry a short name distinct from its long label. */
    std::string path;
    for (size_t i = 0; i < s_pages.size(); i++) {
        if (i) path += "/";
        path += s_pages[i].node->shortName;
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
    lv_obj_set_style_pad_row(pg, 4, 0);
    /* Half a row of run-off under the last one, so the bottom of a scrolled
     * pane doesn't read as content cut off at the edge of the screen. */
    lv_obj_set_style_pad_bottom(pg, SETTINGS_ROW_H / 2, 0);
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

/* Render one node: its own rows first (every contributed block, in order), then
 * a navigation row per child that has anything to render. The page goes on the
 * stack BEFORE the builders run, so a builder that opens a modal or reads the
 * nav state sees a coherent stack. Children are filtered and sorted on a copy,
 * leaving the registry untouched. */
void pushNode(Node* node) {
    lv_obj_t* pg = makePage();
    s_pages.push_back({ pg, node });
    updateHeader();

    for (lcd_fn_t fn : node->builders) if (fn) fn(pg);

    std::vector<Node*> kids;
    for (Node* k : node->kids) if (nodeRenders(k)) kids.push_back(k);
    std::stable_sort(kids.begin(), kids.end(), nodeLess);
    for (Node* k : kids) {
        lv_obj_t* row = lv_button_create(pg);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 38);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x20262e), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_add_event_cb(row, onRowClick, LV_EVENT_CLICKED, k);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, k->label.c_str());
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        /* Navigation rows get a larger face than the in-pane controls (which
         * keep the inherited size) — these are the primary tap targets. */
        lv_obj_set_style_text_font(lbl, lcdFont(LcdFace::UI, lcdPx(16)), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t* ch = lv_label_create(row);
        lv_label_set_text(ch, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(ch, lv_color_hex(0x8a93a0), 0);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -10, 0);
    }
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
    if (n) pushNode(n);
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
    pushNode(&s_root);                 /* root page */
}

/* ---- row scaffolding ---- */

/* Two-column row: a 1/3 label (right-aligned, hugging the divider) and a 2/3
 * control area (left-aligned, so the control sits right next to its label rather
 * than pushed to the far edge). Helpers add the control after addRowLabel(); the
 * fillRowControl() helper stretches a control across the 2/3 where that reads
 * better (dropdown / value / slider group). */
lv_obj_t* makeRow(lv_obj_t* parent, bool compact = false) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, compact ? (SETTINGS_ROW_H * 3) / 4 : SETTINGS_ROW_H);
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

/* Halve a control's vertical padding. Every button and every input field in
 * Settings gets this: the theme sizes them for a finger on a tablet, and on a
 * pane of stacked rows that verticality is what pushes content off the bottom.
 * Read-then-halve rather than a number of our own, so the widget keeps whatever
 * proportions the theme gave it. Horizontal padding is left alone — that is what
 * keeps a label off the edge of its button. */
void halfPadVer(lv_obj_t* w) {
    lv_obj_set_style_pad_top(w,    lv_obj_get_style_pad_top(w,    LV_PART_MAIN) / 2, 0);
    lv_obj_set_style_pad_bottom(w, lv_obj_get_style_pad_bottom(w, LV_PART_MAIN) / 2, 0);
}

/* Every slider readout gets the SAME column, whatever that slider's range is:
 * six digits of the readout's mono face. Sizing each row to its own range put
 * the sliders at a different x on every row, which is exactly what a column of
 * controls must not do. Six covers every range in the tree (the widest is a
 * week in seconds); a value longer than that ellipsizes rather than moving the
 * slider. Measured in the label's own font at the current UI zoom, so the
 * column tracks the zoom like everything else. */
constexpr int NUM_COL_DIGITS = 6;

int32_t numColWidth(lv_obj_t* label) {
    char buf[NUM_COL_DIGITS + 1];
    memset(buf, '0', NUM_COL_DIGITS);
    buf[NUM_COL_DIGITS] = '\0';

    lv_point_t sz;
    lv_text_get_size(&sz, buf, lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                     0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
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

enum BindKind { BK_SWITCH, BK_SLIDER, BK_DROPDOWN, BK_TEXTLBL, BK_TEXTAREA, BK_VALUE,
                BK_WHENKEY };
struct Bind { std::string key; lv_obj_t* w; BindKind kind; bool secret; };
std::vector<Bind> s_binds;

/* A gate key is TRUTHY or it is not — never compared against a value. The
 * firmware publishes gate keys as truthy/empty precisely so no UI has to know
 * what the interesting value is. */
bool truthy(const char* v) { return v && *v && strcmp(v, "0") != 0; }

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
        case BK_WHENKEY:
            if (truthy(val)) lv_obj_remove_flag(b.w, LV_OBJ_FLAG_HIDDEN);
            else             lv_obj_add_flag   (b.w, LV_OBJ_FLAG_HIDDEN);
            break;
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
    /* By callback, never by scope: the lcd task also watches keys of its own on
     * this same task (the backlight, the inactivity timeout, a board's panel
     * keys), and a plain storageUnsubscribe(key) would take those down with it —
     * the setting would then write its key with nothing left to apply it. */
    storageUnsubscribeCb(key.c_str(), bindDispatch);
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

    lv_obj_t* opener = lcdInputGroup() ? lv_group_get_focused(lcdInputGroup()) : nullptr;
    lv_obj_t* ov = lv_obj_create(lv_layer_top());
    lcdSettingsRefocusOnClose(ov, opener);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    s_ed.overlay = ov;

    lv_obj_t* ta = lv_textarea_create(ov);
    halfPadVer(ta);
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

/** Can the focus ring stand on this object right now? */
bool focusable(lv_obj_t* obj, lv_group_t* g) {
    return lv_obj_get_group(obj) == g && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/** The first widget of `root`'s subtree the ring can stand on, in tree order. */
lv_obj_t* firstFocusable(lv_obj_t* root, lv_group_t* g) {
    if (!root) return nullptr;
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* ch = lv_obj_get_child(root, i);
        if (focusable(ch, g)) return ch;
        if (lv_obj_t* deeper = firstFocusable(ch, g)) return deeper;
    }
    return nullptr;
}

/* An overlay is going: put the focus ring back where the overlay took it from,
 * before LVGL's own refocus can guess. See lcdSettingsRefocusOnClose. */
void refocusOnClose(lv_event_t* e) {
    lv_group_t* g = lcdInputGroup();
    if (!g) return;

    /* Only the overlay the ring is standing IN gets to hand it back. A page
     * that closed itself to open something else (a detail page putting its
     * confirmation on a clear screen) is deleted with the ring already in that
     * something else, and must not pull it out from under a dialog that is
     * still on screen. */
    lv_obj_t* focused = lv_group_get_focused(g);
    auto* overlay = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!focused || !lv_obj_is_valid(focused)) return;
    for (lv_obj_t* p = focused; ; p = lv_obj_get_parent(p)) {
        if (p == overlay) break;
        if (p == nullptr) return;           /* the ring is somewhere else */
    }

    auto* opener = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    /* Both checks earn their keep: the opener may have been deleted while the
     * overlay was up, and a freed pointer can be reused by an object that is
     * not in this group. */
    if (opener && lv_obj_is_valid(opener) && focusable(opener, g)) {
        lv_group_focus_obj(opener);
        return;
    }
    /* The opener did not survive — a detail page that closed itself to put its
     * confirmation on a clear screen, a list row a rebuild replaced. Somewhere
     * on the VISIBLE page, chosen here rather than left to LVGL: its walk is
     * over group insertion order, which from this end wraps to the last widget
     * ever added — the bottom row of the pane, scrolled into view, caret and
     * all if it is a text row. The top of the page is where the thing that was
     * being edited was reached from. */
    if (s_pages.empty()) return;
    if (lv_obj_t* home = firstFocusable(s_pages.back().obj, g)) lv_group_focus_obj(home);
}

}  // namespace

/* ================= public registry ================= */

void lcdSettingsRefocusOnClose(lv_obj_t* overlay, lv_obj_t* opener) {
    if (!overlay) return;
    lv_obj_add_event_cb(overlay, refocusOnClose, LV_EVENT_DELETE, opener);
}

void lcdSettingsContribute(const lcd_seg_t* segs, int nsegs, lcd_fn_t fn) {
    if (!segs || nsegs <= 0) return;
    Node* cur = &s_root;
    for (int i = 0; i < nsegs; i++) {
        const lcd_seg_t& s = segs[i];
        if (!s.id || !*s.id) return;
        Node* n = cur->find(s.id);
        if (!n) {
            n = new Node();
            n->id      = s.id;
            n->label   = s.id;
            titleCase(n->label);        /* until somebody names it */
            n->arrival = s_arrival++;
            cur->kids.push_back(n);
        }
        /* Last setter wins, per field. A node nobody names keeps its
         * title-cased id. (The generated contributions all carry the same
         * already-resolved naming — the generator settles it across the staged
         * set — so this only decides between hand-written ones.) */
        if (s.label && *s.label) { n->label = s.label; n->named = true; }
        if (s.shortName && *s.shortName) {
            n->shortName = s.shortName;
            n->shortNamed = true;
        }
        if (s.has_order) { n->hasOrder = true; n->order = s.order; }
        if (!n->shortNamed) n->shortName = n->label;   /* short defaults to the label */
        cur = n;
    }
    if (fn) cur->builders.push_back(fn);
}

/* ================= helpers ================= */

lv_obj_t* lcdSettingSection(lv_obj_t* parent, const char* title) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, title);
    lv_obj_set_style_text_color(l, lv_color_hex(0x6cc0ff), 0);
    /* Half again the body size, in the bold face: a section heading is the only
     * thing that divides a long pane, and at body size and body weight it reads
     * as one more row. 14 px is the sheet's body size. */
    lv_obj_set_style_text_font(l, lcdFont(LcdFace::UI_BOLD, lcdPx(21)), 0);
    lv_obj_set_style_pad_top(l, 8, 0);
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
                           int min, int max, const char* minKey, const char* maxKey) {
    /* A bound the device publishes outranks the one compiled in: the firmware
     * can measure what the hardware in front of it will actually do, and a
     * control offering more than that is offering something it cannot deliver.
     * Read when the row is built — a published capability is established at
     * startup, long before a pane is opened — falling back to the compiled
     * bound while the key does not exist. */
    if (minKey) min = storageGetInt(minKey, min);
    if (maxKey) max = storageGetInt(maxKey, max);
    if (max < min) max = min;

    lv_obj_t* row = makeRow(parent);
    addRowLabel(row, label);

    /* Control group filling the 2/3 column: the slider takes whatever the
     * readout's column leaves, and the readout sits against the right edge (a
     * slider alone hides the exact value). */
    lv_obj_t* grp = lv_obj_create(row);
    lv_obj_remove_style_all(grp);
    lv_obj_set_height(grp, LV_SIZE_CONTENT);
    fillRowControl(grp);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    /* Slider first, so every one of them starts at the same x down the pane. */
    lv_obj_t* s = lv_slider_create(grp);
    fillRowControl(s);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, storageGetInt(key, min), LV_ANIM_OFF);

    /* LVGL draws the knob as a square the height of the track, grown by the knob
     * part's padding, CENTRED on the end of the indicator — so at either bound
     * half of it hangs outside the slider's own box. The theme grows it a further
     * LV_DPX(3) while pressed, which is exactly when a knob is at a bound, hence
     * the last term. Room for that half is the GROUP's padding and its column
     * gap, never a margin on the slider: LVGL's flex adds a grow item's margins
     * when it positions the next item but never counts them in the track it is
     * dividing up, so a margin here silently pushes the readout off the row. */
    int32_t knobHalf = lv_obj_get_style_height(s, LV_PART_MAIN) / 2
                     + lv_obj_get_style_pad_left(s, LV_PART_KNOB) + lcdPx(3);
    lv_obj_set_style_pad_left(grp, knobHalf, 0);
    lv_obj_set_style_pad_column(grp, knobHalf, 0);

    lv_obj_t* num = lv_label_create(grp);
    lv_obj_set_style_text_color(num, lv_color_hex(0xb0b8c0), 0);
    /* Mono, so the value's own digits don't shuffle inside the column as it
     * counts — the readout is a number being watched, not prose. */
    lv_obj_set_style_text_font(num, lcdFont(LcdFace::MONO, lcdPx(14)), 0);
    lv_obj_set_width(num, numColWidth(num));
    lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(num, LV_LABEL_LONG_DOT);
    lv_label_set_text_fmt(num, "%d", storageGetInt(key, min));

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
        halfPadVer(ta);
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
    halfPadVer(d);
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
    halfPadVer(b);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, onButton, LV_EVENT_CLICKED, reinterpret_cast<void*>(onClick));
    return b;
}

/* ---- info groups ----
 * A run of read-only values with one shared label column, sized to the widest
 * label and capped at the third an ordinary row gives its label, and no gap
 * between the lines. LVGL has no cross-sibling max-content, so the width is not
 * knowable while the lines are being built: lcdSettingInfoFit runs a layout pass
 * once they all exist, reads each label's natural width, and writes the widest
 * back to all of them. That is also why the group is three calls rather than one
 * — the caller is what knows the run has ended. */

lv_obj_t* lcdSettingInfo(lv_obj_t* parent) {
    lv_obj_t* g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_width(g, lv_pct(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(g, 8, 0);
    lv_obj_set_style_pad_ver(g, 2, 0);
    lv_obj_set_style_pad_row(g, 0, 0);          /* the lines sit against each other */
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    return g;
}

lv_obj_t* lcdSettingInfoValue(lv_obj_t* group, const char* label, const char* key) {
    lv_obj_t* line = lv_obj_create(group);
    lv_obj_remove_style_all(line);
    lv_obj_set_width(line, lv_pct(100));
    lv_obj_set_height(line, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(line, 10, 0);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    /* Child 0 is the label and child 1 the value — lcdSettingInfoFit walks the
     * group by position, so this order is load-bearing. */
    lv_obj_t* lab = lv_label_create(line);
    lv_label_set_text(lab, label);
    lv_obj_set_style_text_color(lab, lv_color_hex(0x8a93a0), 0);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_RIGHT, 0);

    std::string v = storageGetStr(key, "");
    lv_obj_t* val = lv_label_create(line);
    lv_obj_set_style_text_color(val, lv_color_hex(0xb0b8c0), 0);
    lv_label_set_text(val, v.empty() ? "\xE2\x80\x94" : v.c_str());
    bindAttach(val, key, BK_VALUE);
    /* A long value ellipsizes here rather than taking the stacked label-over-
     * wrapped-value shape a lone value row falls back to: the stack has no left
     * column, and a group whose lines disagreed about that would not be one. */
    lv_obj_set_flex_grow(val, 1);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
#if CONFIG_LCD_SETTINGS_MARQUEE
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), val);
    lv_obj_add_event_cb(val, marqueeFocusCb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(val, marqueeFocusCb, LV_EVENT_DEFOCUSED, nullptr);
#endif
    return line;
}

void lcdSettingInfoFit(lv_obj_t* group) {
    if (!group) return;
    lv_obj_update_layout(group);                /* natural label widths, now real */

    int32_t widest = 0;
    uint32_t n = lv_obj_get_child_count(group);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* line = lv_obj_get_child(group, i);
        if (lv_obj_get_child_count(line) < 1) continue;
        int32_t w = lv_obj_get_width(lv_obj_get_child(line, 0));
        if (w > widest) widest = w;
    }
    /* Never wider than an ordinary row's label column — narrower is the whole
     * point, wider would make the group disagree with the rows around it. */
    int32_t cap = lv_obj_get_content_width(group) / 3;
    if (cap > 0 && widest > cap) widest = cap;
    if (widest <= 0) return;

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* line = lv_obj_get_child(group, i);
        if (lv_obj_get_child_count(line) < 1) continue;
        lv_obj_set_width(lv_obj_get_child(line, 0), widest);
    }
}

lv_obj_t* lcdSettingWhenKey(lv_obj_t* row, const char* key) {
    if (!row || !key || !*key) return row;
    /* Rides the same binding table as every other storage-bound control, so it
     * inherits the subscribe-once / unsubscribe-with-the-last-user lifetime and
     * the tear-down on widget delete. Storage callbacks land on the task that
     * subscribed, and panes are built on the lcd task, so this touches LVGL
     * directly like the rest of the table. */
    bindAttach(row, key, BK_WHENKEY);
    std::string v = storageGetStr(key, "");
    if (truthy(v.c_str())) lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
    else                   lv_obj_add_flag   (row, LV_OBJ_FLAG_HIDDEN);
    return row;
}

/* ---- shared row scaffolding (lcd_settings_priv.h) ----
 * The descriptor runtime builds rows that look exactly like the ones above, so
 * it uses the same primitives rather than a second set that could drift. */

lv_obj_t* lcdSettingsMakeRow(lv_obj_t* parent, bool compact)  { return makeRow(parent, compact); }
void      lcdSettingsHalfPadVer(lv_obj_t* w)                  { halfPadVer(w); }
void      lcdSettingsRowLabel(lv_obj_t* row, const char* txt) { addRowLabel(row, txt); }
void      lcdSettingsFillControl(lv_obj_t* w)                 { fillRowControl(w); }
void      lcdSettingsValueText(lv_obj_t* lbl, const char* v, bool secret) {
    setValueText(lbl, v ? v : "", secret);
}

/* ================= gear program ================= */

/* SettingsApp — a thin LcdApp host around the existing page-stack. onCreate
 * builds the menu tree (settingsOpen) into the app's root; the slash-path
 * registry, the lcdSetting* builders, two-way storage binding, scroll pills and
 * every straddle's pane hook are byte-for-byte the code that shipped before. */
namespace {
class SettingsApp : public LcdApp {
public:
    SettingsApp() : LcdApp({ .name = "Settings", .iconBasename = "gear" }) {}
    void onCreate(lv_obj_t* root) override { settingsOpen(root); }
    void onClose() override {
        s_pages.clear();               /* drop dangling page pointers */
        /* Modals live on lv_layer_top, outside the app's widget tree — the
         * app's teardown never reaches them. The descriptor runtime's (forms,
         * dialogs) and this file's own text editor both go with the app. */
        lcdSettingsDescReset();
        if (s_ed.overlay) { lv_obj_delete(s_ed.overlay); s_ed.overlay = nullptr; }
    }
};
}  // namespace

void lcdSettingsInit(void) {
    s_root.label     = "Settings";
    s_root.shortName = "Settings";
    lcdInstall(new SettingsApp());
}
