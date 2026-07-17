/**
 * recents.cpp — the app switcher (plan §5.5). A modal overlay listing the
 * running set (apps whose root layer currently exists) as horizontal cards. Each
 * card is a half-scale PSRAM snapshot of that app's screen (LcdApp::_captureThumb,
 * taken by the manager the moment the app left the foreground) with its name
 * below — the live miniature, not an icon on a flat tile. An app that has no
 * snapshot yet falls back to its launcher icon. A memory readout (internal /
 * external heap) sits at the top — a genuinely useful on-device diagnostic.
 *
 * Tap a card to switch to it; swipe a card up to terminate that app (stop ->
 * onClose + free root + ledger + snapshot, drop from the running set). Built lazily,
 * repopulated on each show.
 */
#include "shell_internal.h"
#include "stylesheet.h"
#include "lcd_internal.h"   /* lcdScreenW/H, lcdIcon*, lcdInputGroup */
#include "log.h"

#include "esp_heap_caps.h"

#include <vector>

namespace {

lv_obj_t* s_overlay = nullptr;
lv_obj_t* s_mem     = nullptr;
lv_obj_t* s_cards   = nullptr;   /* horizontal card strip */

/* Per-card vertical-swipe drag state. Only one card is touched at a time, so a
 * single set of file-scope cursors mirrors the manager's home-bar drag. The
 * card rides up under the finger via a translate (flex owns its base position,
 * so a plain set_y would be clobbered on the next layout); a release past
 * swipeClosePx terminates the app, anything shorter springs it back, and a
 * press that never moved is a tap that opens the app. */
lv_obj_t* s_dragCard   = nullptr;
LcdApp*   s_dragApp    = nullptr;
int       s_dragStartX = 0;
int       s_dragStartY = 0;
bool      s_dragging   = false;
bool      s_dragMoved  = false;

int headerH() { return 22; }

/* px the finger may wander and still count as a tap, not a swipe/scroll */
const int kTapSlop = 12;

lv_point_t activePoint() {
    lv_point_t p = { 0, 0 };
    lv_indev_t* ind = lv_indev_active();
    if (ind) lv_indev_get_point(ind, &p);
    return p;
}
int activePointerY() { return activePoint().y; }

void updateMem() {
    if (!s_mem) return;
    size_t iFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t iTot  = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t eFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t eTot  = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    lv_label_set_text_fmt(s_mem, "RAM  int %u/%uK   ext %u/%uK",
                          (unsigned)(iFree / 1024), (unsigned)(iTot / 1024),
                          (unsigned)(eFree / 1024), (unsigned)(eTot / 1024));
}

/* Spring a cancelled drag back to its resting place (translate 0, full opa). */
void springCard(lv_obj_t* card) {
    if (!card) return;
    lv_anim_delete(card, nullptr);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, card);
    lv_anim_set_exec_cb(&a, [](void* v, int32_t y) {
        lv_obj_set_style_translate_y((lv_obj_t*)v, y, 0); });
    lv_anim_set_values(&a, lv_obj_get_style_translate_y(card, LV_PART_MAIN), 0);
    lv_anim_set_duration(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_obj_set_style_opa(card, LV_OPA_COVER, 0);
}

void onCardPressed(lv_event_t* e) {
    s_dragCard   = (lv_obj_t*)lv_event_get_target(e);
    s_dragApp    = static_cast<LcdApp*>(lv_event_get_user_data(e));
    lv_anim_delete(s_dragCard, nullptr);
    lv_point_t p = activePoint();
    s_dragStartX = p.x;
    s_dragStartY = p.y;
    s_dragging   = true;
    s_dragMoved  = false;
}

void onCardPressing(lv_event_t* e) {
    if (!s_dragging) return;
    lv_obj_t* card = (lv_obj_t*)lv_event_get_target(e);
    int dy = activePointerY() - s_dragStartY;
    if (dy > 0) dy = 0;                 /* up only — the card never droops down */
    if (dy < -4) s_dragMoved = true;
    lv_obj_set_style_translate_y(card, dy, 0);
    /* Fade as it lifts so the dismissal reads as "throwing it away". */
    int lift = -dy;
    lv_opa_t opa = (lift >= 175) ? (lv_opa_t)80 : (lv_opa_t)(LV_OPA_COVER - lift);
    lv_obj_set_style_opa(card, opa, 0);
}

void onCardReleased(lv_event_t* e) {
    if (!s_dragging) return;
    s_dragging = false;
    lv_obj_t* card = (lv_obj_t*)lv_event_get_target(e);
    LcdApp*   app  = s_dragApp;
    bool realRelease = (lv_event_get_code(e) == LV_EVENT_RELEASED);
    int  lift = -lv_obj_get_style_translate_y(card, LV_PART_MAIN);

    /* Total finger travel decides tap vs gesture: a horizontal scroll between
     * cards ends in RELEASED too (the strip eats the drag, so s_dragMoved — which
     * only tracks our vertical lift — stays false), and must NOT open the app. */
    lv_point_t p = activePoint();
    int dx = p.x - s_dragStartX, dy = p.y - s_dragStartY;
    bool wandered = (dx < -kTapSlop || dx > kTapSlop || dy < -kTapSlop || dy > kTapSlop);

    /* Committed upward swipe -> terminate the app. */
    if (realRelease && s_dragMoved && lift >= lcdStyle().recents.swipeClosePx) {
        if (app) app->stop();
        bool any = false;
        for (auto* a : shellApps()) if (a->root()) { any = true; break; }
        /* Last one closed: leave recents up and let shellNavigate's recents-visible
         * path reveal the launcher (it no-ops if recents is already hidden). */
        if (!any) { shellNavigate(NavIntent::HOME); return; }
        shellRecentsShow();   /* rebuild the strip without the closed app */
        return;
    }
    /* A still tap (no scroll, no drag) -> switch to the app. */
    if (realRelease && !wandered) {
        shellRecentsHide();
        if (app) shellOpenApp(app);
        return;
    }
    /* Scroll, short drag, or lost press -> snap back. */
    springCard(card);
}

void addCard(LcdApp* app) {
    const LcdStyle& st = lcdStyle();
    int thumbW = lcdScreenW() / 2;                  /* the 320x240 screen, halved */
    int thumbH = lcdScreenH() / 2;
    int cardH  = thumbH + 24;                        /* thumbnail + name row */

    lv_obj_t* card = lv_button_create(s_cards);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, thumbW, cardH);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A2028), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, onCardPressed,  LV_EVENT_PRESSED,    app);
    lv_obj_add_event_cb(card, onCardPressing, LV_EVENT_PRESSING,   app);
    lv_obj_add_event_cb(card, onCardReleased, LV_EVENT_RELEASED,   app);
    lv_obj_add_event_cb(card, onCardReleased, LV_EVENT_PRESS_LOST, app);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), card);

    lv_obj_t* thumb = lv_image_create(card);
    lv_obj_align(thumb, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_size(thumb, thumbW, thumbH);
    lv_obj_set_style_radius(thumb, 6, 0);
    lv_obj_set_style_clip_corner(thumb, true, 0);
    if (app->_thumb()) {
        /* Fit the live snapshot into the card, keeping its aspect ratio. */
        lv_image_set_src(thumb, app->_thumb());
        lv_image_set_inner_align(thumb, LV_IMAGE_ALIGN_CONTAIN);
    } else {
        /* No snapshot yet (never minimised) — fall back to the launcher icon,
         * rasterized at the recents icon size. */
        const char* base = app->cfg().iconBasename ? app->cfg().iconBasename : "";
        int px = lcdPx(lcdStyle().recents.iconPx);
        const lv_image_dsc_t* dsc = lcdIconDsc(base, px);
        if (dsc) {
            lv_image_set_src(thumb, dsc);
            lv_image_set_inner_align(thumb, LV_IMAGE_ALIGN_CENTER);
        } else {
            lcdIconRequest(base, px);
        }
    }

    lv_obj_t* name = lv_label_create(card);
    lv_label_set_text(name, app->cfg().name ? app->cfg().name : "");
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    if (st.recents.titleFont) lv_obj_set_style_text_font(name, st.recents.titleFont, 0);
    lv_obj_set_width(name, thumbW - 12);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -4);
}

void build() {
    const LcdStyle& st = lcdStyle();
    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_pos(s_overlay, 0, st.statusBar.h);
    lv_obj_set_size(s_overlay, lcdScreenW(), lcdScreenH() - st.statusBar.h);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0C0F12), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_mem = lv_label_create(s_overlay);
    lv_obj_align(s_mem, LV_ALIGN_TOP_LEFT, 8, 4);
    lv_obj_set_style_text_color(s_mem, lv_color_hex(0x9098A0), 0);
    if (st.core.font) lv_obj_set_style_text_font(s_mem, st.core.font, 0);

    s_cards = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_cards);
    lv_obj_set_pos(s_cards, 0, headerH());
    lv_obj_set_size(s_cards, lcdScreenW(), lcdScreenH() - st.statusBar.h - headerH());
    lv_obj_set_style_bg_opa(s_cards, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_cards, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_cards, 10, 0);
    lv_obj_set_style_pad_left(s_cards, 10, 0);
    lv_obj_set_scroll_dir(s_cards, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_cards, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_cards, LV_SCROLLBAR_MODE_OFF);
}

}  // namespace

void shellRecentsShow(void) {
    if (!s_overlay) build();
    s_dragging = false; s_dragCard = nullptr; s_dragApp = nullptr;
    lv_obj_clean(s_cards);
    int n = 0;
    for (auto* a : shellApps())
        if (a->root()) { addCard(a); n++; }
    if (n == 0) {
        lv_obj_t* empty = lv_label_create(s_cards);
        lv_label_set_text(empty, "No running apps");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x9098A0), 0);
    }
    updateMem();
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);   /* above the program layers */
    /* Fade in over 200ms (the app fades out underneath, driven by the manager). */
    lv_anim_delete(s_overlay, nullptr);
    lv_obj_set_style_opa(s_overlay, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_overlay);
    lv_anim_set_exec_cb(&a, [](void* v, int32_t op) { lv_obj_set_style_opa((lv_obj_t*)v, (lv_opa_t)op, 0); });
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 200);
    lv_anim_start(&a);
    dbg("recents: %d running\n", n);
}

void shellRecentsHide(void) {
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool shellRecentsVisible(void) {
    return s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}
