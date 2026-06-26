/**
 * launcher.cpp — the phone shell's paged icon grid.
 *
 * Layout (stylesheet-driven): a horizontal pager of pages, each a flex-wrap grid
 * of tiles, with a page-indicator dot row at the bottom. With ~7 apps one page
 * suffices, but the mechanism is paged from day one (Config::launcherPage places
 * a tile; a new page is created on demand). The pager scrolls by horizontal
 * trackball drag (pointer indev) and by lcdScroll() edge-pan.
 *
 * Tiles are added by shellLauncherAddTile(), called from lcdInstall()
 * (lcd_app.cpp) for every installed LcdApp.
 */
#include "shell_internal.h"
#include "stylesheet.h"
#include "lcd_internal.h"   /* lcdScreenW/H, lcdInputGroup, lcdIcon* */
#include "log.h"

#include <vector>
#include <string>

namespace {

lv_obj_t* s_root  = nullptr;   /* launcher container (lower sibling of programs) */
lv_obj_t* s_pager = nullptr;   /* horizontally-scrollable page strip */
lv_obj_t* s_dots  = nullptr;   /* page-indicator dot row */
std::vector<lv_obj_t*> s_pages;

struct Tile { LcdApp* app; lv_obj_t* img; std::string basename; };
std::vector<Tile> s_tiles;

int pageW() { return lcdScreenW(); }
int dotRowH() { return 16; }
int pageH() { return (lcdScreenH() - lcdStyle().statusBar.h) - dotRowH(); }

int currentPage() {
    if (!s_pager || s_pages.empty()) return 0;
    int w = pageW();
    if (w <= 0) return 0;
    int p = (lv_obj_get_scroll_x(s_pager) + w / 2) / w;
    if (p < 0) p = 0;
    if (p >= (int)s_pages.size()) p = (int)s_pages.size() - 1;
    return p;
}

void updateDots() {
    if (!s_dots) return;
    const LcdStyle& st = lcdStyle();
    int cur = currentPage();
    lv_obj_clean(s_dots);
    for (size_t i = 0; i < s_pages.size(); i++) {
        lv_obj_t* d = lv_obj_create(s_dots);
        lv_obj_remove_style_all(d);
        bool active = ((int)i == cur);
        lv_obj_set_size(d, active ? st.launcher.dotActive : st.launcher.dotSize,
                            st.launcher.dotSize);
        lv_obj_set_style_radius(d, st.launcher.dotSize / 2, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(active ? 0xE0E0E0 : 0x404850), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    }
    /* One page: no indicator clutter. */
    if (s_pages.size() <= 1) lv_obj_add_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
    else                     lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
}

void onPagerScroll(lv_event_t*) { updateDots(); }

lv_obj_t* ensurePage(int n) {
    const LcdStyle& st = lcdStyle();
    while ((int)s_pages.size() <= n) {
        lv_obj_t* pg = lv_obj_create(s_pager);
        lv_obj_remove_style_all(pg);
        lv_obj_set_size(pg, pageW(), pageH());
        lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(pg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_top(pg, st.launcher.padTop, 0);
        lv_obj_set_style_pad_left(pg, st.launcher.padLeft, 0);
        lv_obj_set_style_pad_row(pg, st.launcher.padRow, 0);
        lv_obj_set_style_pad_column(pg, st.launcher.padCol, 0);
        s_pages.push_back(pg);
    }
    return s_pages[n];
}

void onTileClick(lv_event_t* e) {
    LcdApp* app = static_cast<LcdApp*>(lv_event_get_user_data(e));
    if (app) { dbg("tile click '%s'\n", app->cfg().name); shellOpenApp(app); }
}

}  // namespace

lv_obj_t* shellLauncherRoot(void) { return s_root; }

void shellLauncherInit(lv_obj_t* screen) {
    const LcdStyle& st = lcdStyle();

    s_root = lv_obj_create(screen);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_pos(s_root, 0, st.statusBar.h);
    lv_obj_set_size(s_root, lcdScreenW(), lcdScreenH() - st.statusBar.h);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(st.launcher.bg), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_pager = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_pager);
    lv_obj_set_pos(s_pager, 0, 0);
    lv_obj_set_size(s_pager, lcdScreenW(), pageH());
    lv_obj_set_style_bg_opa(s_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_pager, LV_FLEX_FLOW_ROW);
    lv_obj_add_flag(s_pager, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_pager, onPagerScroll, LV_EVENT_SCROLL_END, nullptr);

    s_dots = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, lcdScreenW(), dotRowH());
    lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_dots, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dots, 6, 0);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_SCROLLABLE);

    ensurePage(0);
    updateDots();
}

void shellLauncherAddTile(LcdApp* app) {
    if (!s_pager || !app) return;
    const LcdStyle& st = lcdStyle();
    lv_obj_t* page = ensurePage(app->cfg().launcherPage);

    lv_obj_t* tile = lv_button_create(page);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, st.launcher.tileW, st.launcher.tileH);
    lv_obj_add_event_cb(tile, onTileClick, LV_EVENT_CLICKED, app);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), tile);

    lv_obj_t* img = lv_image_create(tile);   /* src set when the icon loads */
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t* lbl = lv_label_create(tile);
    lv_label_set_text(lbl, app->cfg().name ? app->cfg().name : "");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    if (st.launcher.labelFont) lv_obj_set_style_text_font(lbl, st.launcher.labelFont, 0);
    lv_obj_set_width(lbl, st.launcher.tileW);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -2);

    const char* base = app->cfg().iconBasename ? app->cfg().iconBasename : "";
    info("add tile '%s' (icon '%s', page %d)\n", app->cfg().name ? app->cfg().name : "",
         base, app->cfg().launcherPage);
    s_tiles.push_back({ app, img, base });
    updateDots();

    lcdIconRequest(base);   /* -> lcdLauncherIconLoaded */
}

void lcdLauncherIconLoaded(const char* basename) {
    if (!basename) return;
    char src[160];
    lcdIconSrc(basename, src, sizeof(src));
    for (auto& t : s_tiles)
        if (t.img && t.basename == basename) lv_image_set_src(t.img, src);
    lcdBootSettleKick();   /* an icon landed — push the boot backlight reveal out */
}

void lcdLauncherReload(void) {
    for (auto& t : s_tiles) {
        if (lcdIconReady(t.basename.c_str())) {
            char src[160];
            lcdIconSrc(t.basename.c_str(), src, sizeof(src));
            if (t.img) lv_image_set_src(t.img, src);
        } else {
            lcdIconRequest(t.basename.c_str());
        }
    }
}
