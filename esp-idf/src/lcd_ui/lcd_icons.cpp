/**
 * lcd_icons.cpp — runtime SVG icon rasterizer + RAM cache + off-task loader.
 *
 * Straddles ship their icon *sources* (`.svg`) to /fixed/icons/<name>.svg; the
 * device rasterizes them at exactly the requested pixel size with nanosvg. There
 * is no build-time bucket pipeline and no fixed resolution — a tile asks for its
 * icon at `iconPx × uiScale` and gets a crisp raster, so any tile size / runtime
 * zoom is just arithmetic.
 *
 * The lcd task must not do flash I/O or heavy CPU (it runs itsPoll + the LVGL
 * render loop), so a dedicated loader task reads the SVG, parses it (nsvgParse
 * mutates its input, so we copy out of the mmap window first), rasterizes to
 * RGBA8888, converts to LVGL RGB565A8 (the panel format, with an alpha plane),
 * and hands the finished lv_image_dsc_t back to the lcd task via lcdRun(). The
 * lcd task drops it into an in-RAM cache keyed (basename, px) and calls
 * lcdLauncherIconLoaded(); the launcher sets it as the tile image directly
 * (lv_image_set_src(img, dsc)) — no lv_fs indirection.
 *
 * The cache is touched only on the lcd task; the loader only uses its request
 * queue + fs + lcdRun. No locks.
 */
#include "lcd_internal.h"

#include "fs.h"
#include "log.h"
#include "compat.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"

/* nanosvg: declarations only here (bodies live in nanosvg_impl.c). */
#include "nanosvg.h"
#include "nanosvgrast.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>
#include <string>
#include <unordered_map>
#include <sys/stat.h>

#define LCD_ICON_QUEUE_DEPTH 16
#define LCD_ICON_MAX_SVG     (128 * 1024)   /* a launcher icon SVG is < a few KB */
#define LCD_ICON_MAX_PX      256            /* sanity clamp on the raster size */

namespace {

/* A decoded icon: an LVGL image descriptor pointing at a PSRAM RGB565A8 buffer.
 * Heap-allocated so &dsc stays stable across cache growth (lv_image holds it). */
struct Icon {
    lv_image_dsc_t dsc{};
    uint8_t*       pixels = nullptr;   /* colour plane (w*h*2) + alpha plane (w*h) */
};

std::unordered_map<std::string, Icon*> s_cache;   /* lcd task only; key "base@px" */
QueueHandle_t s_loadQ = nullptr;

std::string keyOf(const char* base, int px) { return std::string(base) + "@" + std::to_string(px); }

/* ---- loader task (off the lcd task): read → parse → raster → convert ---- */

struct LoadReq   { char base[40]; int px; };
struct LoadedMsg { char base[40]; int px; Icon* icon; };

/* Build the RGB565A8 icon from /fixed/icons/<base>.svg at `px`. Runs on the
 * loader task. Returns nullptr on any failure (missing file, parse error, OOM).*/
Icon* buildIcon(const char* base, int px) {
    if (px < 1) px = 1;
    if (px > LCD_ICON_MAX_PX) px = LCD_ICON_MAX_PX;

    char path[160];
    snprintf(path, sizeof(path), "/fixed/icons/%s.svg", base);

    struct stat st;
    if (fs_stat(path, &st) != 0) { warn("icon missing: %s\n", path); return nullptr; }
    long sz = (long)st.st_size;
    if (sz <= 0 || sz > LCD_ICON_MAX_SVG) { warn("icon bad size %ld: %s\n", sz, path); return nullptr; }

    /* nsvgParse mutates + NUL-terminates its input: read into a private buffer. */
    char* svg = (char*)malloc((size_t)sz + 1);
    if (!svg) { warn("icon svg alloc fail\n"); return nullptr; }
    int f = fs_open(path, "rb");
    if (f < 0) { free(svg); warn("icon open fail: %s\n", path); return nullptr; }
    size_t got = fs_read(svg, 1, (size_t)sz, f);
    fs_close(f);
    if (got != (size_t)sz) { free(svg); warn("icon short read: %s\n", path); return nullptr; }
    svg[sz] = '\0';

    NSVGimage* image = nsvgParse(svg, "px", 96.0f);
    free(svg);
    if (!image || image->width <= 0 || image->height <= 0) {
        if (image) nsvgDelete(image);
        warn("icon parse fail: %s\n", path);
        return nullptr;
    }

    /* Fit the icon into a px×px box, preserving aspect (icons are ~square). */
    float dim   = image->width > image->height ? image->width : image->height;
    float scale = (float)px / dim;

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    uint8_t* rgba = (uint8_t*)malloc((size_t)px * px * 4);
    if (!rast || !rgba) {
        if (rast) nsvgDeleteRasterizer(rast);
        free(rgba); nsvgDelete(image);
        warn("icon raster alloc fail\n");
        return nullptr;
    }
    memset(rgba, 0, (size_t)px * px * 4);
    nsvgRasterize(rast, image, 0, 0, scale, rgba, px, px, px * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    /* RGBA8888 → LVGL RGB565A8: colour plane (px*px*2, little-endian RGB565)
     * immediately followed by the alpha plane (px*px). */
    size_t colorBytes = (size_t)px * px * 2;
    size_t total      = colorBytes + (size_t)px * px;   /* + alpha plane */
    uint8_t* out = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!out) { free(rgba); warn("icon out alloc fail %u B\n", (unsigned)total); return nullptr; }
    uint8_t* alpha = out + colorBytes;
    for (int i = 0; i < px * px; i++) {
        uint8_t r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        out[i * 2 + 0] = (uint8_t)(v & 0xFF);
        out[i * 2 + 1] = (uint8_t)(v >> 8);
        alpha[i] = a;
    }
    free(rgba);

    Icon* ic = new (std::nothrow) Icon();
    if (!ic) { free(out); return nullptr; }
    ic->pixels = out;
    ic->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    ic->dsc.header.cf     = LV_COLOR_FORMAT_RGB565A8;
    ic->dsc.header.w      = (uint32_t)px;
    ic->dsc.header.h      = (uint32_t)px;
    ic->dsc.header.stride = (uint32_t)px * 2;
    ic->dsc.data_size     = (uint32_t)total;
    ic->dsc.data          = out;
    return ic;
}

/* runs on the lcd task via lcdRun() */
void onLoaded(void* arg) {
    auto* m = static_cast<LoadedMsg*>(arg);
    if (m->icon) {
        std::string k = keyOf(m->base, m->px);
        auto it = s_cache.find(k);
        if (it != s_cache.end()) {                 /* replace (rare) */
            free(it->second->pixels);
            delete it->second;
        }
        s_cache[k] = m->icon;
        lcdLauncherIconLoaded(m->base, m->px);
    }
    delete m;
}

void loaderFn(void*) {
    LoadReq req;
    for (;;) {
        if (xQueueReceive(s_loadQ, &req, portMAX_DELAY) != pdTRUE) continue;
        Icon* ic = buildIcon(req.base, req.px);
        auto* m = new LoadedMsg{};
        safeStrncpy(m->base, req.base, sizeof(m->base));
        m->px = req.px;
        m->icon = ic;
        lcdRun(onLoaded, m);
    }
}

}  // namespace

/* ---- internal API ---- */

void lcdIconsInit(void) {
    s_loadQ = xQueueCreate(LCD_ICON_QUEUE_DEPTH, sizeof(LoadReq));
    /* nanosvg parse + raster want headroom; give the loader a PSRAM stack. */
    spawnTask(loaderFn, "lcd_load", 8192, nullptr, 1, 1, STACK_PSRAM);
}

bool lcdIconReady(const char* basename, int px) {
    if (!basename) return false;
    return s_cache.count(keyOf(basename, px)) > 0;
}

const lv_image_dsc_t* lcdIconDsc(const char* basename, int px) {
    if (!basename) return nullptr;
    auto it = s_cache.find(keyOf(basename, px));
    return it == s_cache.end() ? nullptr : &it->second->dsc;
}

void lcdIconRequest(const char* basename, int px) {
    if (!basename || !*basename || px < 1) return;
    if (lcdIconReady(basename, px)) { lcdLauncherIconLoaded(basename, px); return; }
    LoadReq req{};
    safeStrncpy(req.base, basename, sizeof(req.base));
    req.px = px;
    if (s_loadQ && xQueueSend(s_loadQ, &req, 0) != pdTRUE) warn("icon load queue full\n");
}

void lcdIconsReset(void) {
    /* Drop every cached raster (a zoom change re-requests at the new size). Any
     * lv_image still pointing at a freed dsc must be re-sourced before the next
     * redraw — the launcher reload does exactly that. Lcd task only. */
    for (auto& kv : s_cache) { free(kv.second->pixels); delete kv.second; }
    s_cache.clear();
}
