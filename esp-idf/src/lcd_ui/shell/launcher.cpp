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

lv_obj_t* s_root   = nullptr;  /* launcher container (lower sibling of programs) */
lv_obj_t* s_pager  = nullptr;  /* horizontally-scrollable page strip */
lv_obj_t* s_dots   = nullptr;  /* page-indicator dot row */
lv_obj_t* s_screen = nullptr;  /* the screen we build into (for rebuild-on-zoom) */
std::vector<lv_obj_t*> s_pages;

struct Tile { LcdApp* app; lv_obj_t* img; std::string basename; };
std::vector<Tile> s_tiles;
std::vector<int>  s_pageFill;   /* tiles placed on each page (for spill) */

/* A sheet length scaled by the runtime UI zoom. */

int pageW() { return lcdScreenW(); }
int dotRowH() { return lcdPx(16); }
/* Icon raster size for the current tile + zoom: the sheet base × UI scale. */
int iconPx() { return lcdPx(lcdStyle().launcher.iconPx); }
int pageH() { return (lcdScreenH() - lcdStyle().statusBar.h) - dotRowH(); }

/* Derived grid: tiles size from the viewport, not the sheet (plan §4). cols =
 * floor(usableW / minTile); tileW fills the row; tileH from content (icon +
 * label + pads). Capacity = cols × rows drives page spill. Recomputed on build /
 * zoom so a wider panel or a bigger zoom simply reflows. */
struct Grid { int cols, rows, tileW, tileH, cap; };
Grid gridFor() {
    const LcdStyle& st = lcdStyle();
    int padL = lcdPx(st.launcher.padLeft), padT = lcdPx(st.launcher.padTop);
    int padC = lcdPx(st.launcher.padCol),  padR = lcdPx(st.launcher.padRow);
    int minT = lcdPx(st.launcher.minTilePx);
    if (minT < 1) minT = 1;

    int availW = pageW() - 2 * padL;
    int cols = availW / minT;
    if (cols < 1) cols = 1;
    int tileW = (availW - (cols - 1) * padC) / cols;

    int labelH = st.launcher.labelFont ? lv_font_get_line_height(st.launcher.labelFont) : lcdPx(14);
    int tileH  = iconPx() + labelH + lcdPx(10);   /* icon + label + internal pads */

    int availH = pageH() - 2 * padT;
    int rows = (availH + padR) / (tileH + padR);
    if (rows < 1) rows = 1;

    return { cols, rows, tileW, tileH, cols * rows };
}

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
        lv_obj_set_size(d, lcdPx(active ? st.launcher.dotActive : st.launcher.dotSize),
                            lcdPx(st.launcher.dotSize));
        lv_obj_set_style_radius(d, lcdPx(st.launcher.dotSize) / 2, 0);
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
        lv_obj_set_style_pad_top(pg, lcdPx(st.launcher.padTop), 0);
        lv_obj_set_style_pad_left(pg, lcdPx(st.launcher.padLeft), 0);
        lv_obj_set_style_pad_row(pg, lcdPx(st.launcher.padRow), 0);
        lv_obj_set_style_pad_column(pg, lcdPx(st.launcher.padCol), 0);
        s_pages.push_back(pg);
        s_pageFill.push_back(0);
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
    s_screen = screen;

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
    lv_obj_set_style_pad_column(s_dots, lcdPx(6), 0);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_SCROLLABLE);

    s_pageFill.clear();
    ensurePage(0);
    updateDots();
}

void shellLauncherAddTile(LcdApp* app) {
    if (!s_pager || !app) return;
    const LcdStyle& st = lcdStyle();
    Grid g = gridFor();

    /* Place on the requested page, spilling to the next once a page is full so
     * launcherPage stays an app-author *request*, not a hard slot. */
    int page = app->cfg().launcherPage;
    if (page < 0) page = 0;
    ensurePage(page);
    while (page < (int)s_pageFill.size() && s_pageFill[page] >= g.cap) page++;
    lv_obj_t* pageObj = ensurePage(page);
    s_pageFill[page]++;

    /* Tile: a flex-column button (icon over label), content-tall, so internals
     * flow with padding instead of the old magic TOP_MID/BOTTOM_MID offsets. */
    lv_obj_t* tile = lv_button_create(pageObj);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, g.tileW, g.tileH);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tile, lcdPx(4), 0);
    lv_obj_set_style_pad_top(tile, lcdPx(4), 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tile, onTileClick, LV_EVENT_CLICKED, app);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), tile);

    lv_obj_t* img = lv_image_create(tile);   /* src set when the icon loads */
    lv_obj_set_size(img, iconPx(), iconPx());   /* stable slot before the raster lands */

    lv_obj_t* lbl = lv_label_create(tile);
    lv_label_set_text(lbl, app->cfg().name ? app->cfg().name : "");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    if (st.launcher.labelFont) lv_obj_set_style_text_font(lbl, st.launcher.labelFont, 0);
    lv_obj_set_width(lbl, g.tileW);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    const char* base = app->cfg().iconBasename ? app->cfg().iconBasename : "";
    info("add tile '%s' (icon '%s', page %d)\n", app->cfg().name ? app->cfg().name : "",
         base, page);
    s_tiles.push_back({ app, img, base });
    updateDots();

    lcdIconRequest(base, iconPx());   /* -> lcdLauncherIconLoaded */
}

void lcdLauncherIconLoaded(const char* basename, int px) {
    if (!basename || px != iconPx()) return;   /* stale (pre-zoom) size: ignore */
    const lv_image_dsc_t* dsc = lcdIconDsc(basename, px);
    if (!dsc) return;
    for (auto& t : s_tiles)
        if (t.img && t.basename == basename) lv_image_set_src(t.img, dsc);
    lcdBootSettleKick();   /* an icon landed — push the boot backlight reveal out */
}

void lcdLauncherReload(void) {
    int px = iconPx();
    for (auto& t : s_tiles) {
        const lv_image_dsc_t* dsc = lcdIconDsc(t.basename.c_str(), px);
        if (dsc) { if (t.img) lv_image_set_src(t.img, dsc); }
        else       lcdIconRequest(t.basename.c_str(), px);
    }
}

void shellLauncherRebuild(void) {
    if (!s_screen) return;
    /* Tear the launcher down and rebuild it at the current scale (new tile
     * geometry + label font). Deleting s_root frees every tile, so no widget
     * holds an old-size icon after this — the icon cache can then be reset
     * (guard: recents may still show icon dscs). New tiles re-request icons at
     * the new px. Runs on the lcd task. */
    if (s_root) lv_obj_delete(s_root);
    s_root = s_pager = s_dots = nullptr;
    s_pages.clear();
    s_pageFill.clear();
    s_tiles.clear();
    if (!shellRecentsVisible()) lcdIconsReset();
    shellLauncherInit(s_screen);
    for (auto* a : shellApps()) shellLauncherAddTile(a);
}
