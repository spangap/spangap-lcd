/**
 * lcd_icons.cpp — icon RAM cache + lv_fs driver + off-task loader.
 *
 * The lcd task must not do flash I/O (it runs itsPoll). So icon .bin files are
 * read by a dedicated loader task — which has no itsPoll loop, so the fs
 * proxy's pickup-wait can't desync anything — and the bytes are handed to the
 * lcd task via lcdRun(). The lcd task drops them into an in-RAM cache, and a
 * tiny lv_fs driver ('D') serves LVGL's image decoder straight from that cache
 * (pure memory, zero flash on the lcd task). Standard
 * lv_image_set_src("D:/fixed/lcd/icons/<res>/<name>.bin") then just works.
 *
 * The cache + resolution string are touched only on the lcd task; the loader
 * only uses its request queue + fs + lcdRun. No locks.
 */
#include "lcd_internal.h"

#include "fs.h"
#include "storage.h"
#include "log.h"
#include "compat.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <sys/stat.h>

#define LCD_ICON_QUEUE_DEPTH 16
#define LCD_ICON_MAX_BYTES   (256 * 1024)

namespace {

struct Blob { uint8_t* data = nullptr; size_t size = 0; };

std::unordered_map<std::string, Blob> s_cache;   /* lcd task only; key = abs path */
std::string  s_res = "40x40";                    /* lcd task only */
lv_fs_drv_t  s_drv;
QueueHandle_t s_loadQ = nullptr;

/* Absolute fs path (no drive letter) for the loader + cache key. */
void absPath(const char* basename, char* out, size_t outLen) {
    snprintf(out, outLen, "/fixed/lcd/icons/%s/%s.bin", s_res.c_str(), basename);
}

/* ---- lv_fs driver over the RAM cache (runs on the lcd task during decode) -- */

struct IconCursor { const uint8_t* data; size_t size; size_t pos; };

void* fsOpen(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
    if (mode & LV_FS_MODE_WR) return nullptr;
    auto it = s_cache.find(path);
    if (it == s_cache.end()) return nullptr;     /* not preloaded yet */
    return new IconCursor{ it->second.data, it->second.size, 0 };
}

lv_fs_res_t fsClose(lv_fs_drv_t*, void* fp) {
    delete static_cast<IconCursor*>(fp);
    return LV_FS_RES_OK;
}

lv_fs_res_t fsRead(lv_fs_drv_t*, void* fp, void* buf, uint32_t btr, uint32_t* br) {
    auto* c = static_cast<IconCursor*>(fp);
    size_t avail = c->size - c->pos;
    size_t n = btr < avail ? btr : avail;
    memcpy(buf, c->data + c->pos, n);
    c->pos += n;
    *br = (uint32_t)n;
    return LV_FS_RES_OK;
}

lv_fs_res_t fsSeek(lv_fs_drv_t*, void* fp, uint32_t pos, lv_fs_whence_t whence) {
    auto* c = static_cast<IconCursor*>(fp);
    size_t base = (whence == LV_FS_SEEK_CUR) ? c->pos
                : (whence == LV_FS_SEEK_END) ? c->size : 0;
    size_t np = base + pos;
    c->pos = np > c->size ? c->size : np;
    return LV_FS_RES_OK;
}

lv_fs_res_t fsTell(lv_fs_drv_t*, void* fp, uint32_t* p) {
    *p = (uint32_t)static_cast<IconCursor*>(fp)->pos;
    return LV_FS_RES_OK;
}

/* ---- loader task (off the lcd task) ---- */

struct LoadReq    { char path[160]; char basename[40]; };
struct LoadedMsg  { char path[160]; char basename[40]; uint8_t* data; size_t size; };

/* runs on the lcd task via lcdRun() */
void onLoaded(void* arg) {
    auto* m = static_cast<LoadedMsg*>(arg);
    if (m->data) {
        Blob& b = s_cache[m->path];
        if (b.data) free(b.data);                /* replace (rare) */
        b.data = m->data;
        b.size = m->size;
        lcdLauncherIconLoaded(m->basename);
    }
    delete m;
}

void loaderFn(void*) {
    LoadReq req;
    for (;;) {
        if (xQueueReceive(s_loadQ, &req, portMAX_DELAY) != pdTRUE) continue;

        struct stat st;
        if (fs_stat(req.path, &st) != 0) { warn("icon missing: %s\n", req.path); continue; }
        long sz = (long)st.st_size;
        if (sz <= 0 || sz > LCD_ICON_MAX_BYTES) {
            warn("icon bad size %ld: %s\n", sz, req.path); continue;
        }
        auto* buf = (uint8_t*)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
        if (!buf) { warn("icon alloc fail %ld B\n", sz); continue; }

        int f = fs_open(req.path, "rb");
        if (f < 0) { free(buf); warn("icon open fail: %s\n", req.path); continue; }
        size_t got = fs_read(buf, 1, (size_t)sz, f);
        fs_close(f);
        if (got != (size_t)sz) { free(buf); warn("icon short read: %s\n", req.path); continue; }

        auto* m = new LoadedMsg{};
        safeStrncpy(m->path,     req.path,     sizeof(m->path));
        safeStrncpy(m->basename, req.basename, sizeof(m->basename));
        m->data = buf;
        m->size = (size_t)sz;
        lcdRun(onLoaded, m);
    }
}

}  // namespace

/* ---- internal API ---- */

void lcdIconsInit(void) {
    s_res = storageGetStr("s.lcd.icon_res", "40x40");
    if (s_res.empty()) s_res = "40x40";

    lv_fs_drv_init(&s_drv);
    s_drv.letter   = 'D';
    s_drv.open_cb  = fsOpen;
    s_drv.close_cb = fsClose;
    s_drv.read_cb  = fsRead;
    s_drv.seek_cb  = fsSeek;
    s_drv.tell_cb  = fsTell;
    lv_fs_drv_register(&s_drv);

    s_loadQ = xQueueCreate(LCD_ICON_QUEUE_DEPTH, sizeof(LoadReq));
    spawnTask(loaderFn, "lcd_load", 4096, nullptr, 1, 1, STACK_PSRAM);
}

const char* lcdIconSrc(const char* basename, char* out, size_t outLen) {
    snprintf(out, outLen, "D:/fixed/lcd/icons/%s/%s.bin", s_res.c_str(), basename);
    return out;
}

bool lcdIconReady(const char* basename) {
    char abs[160];
    absPath(basename, abs, sizeof(abs));
    return s_cache.count(abs) > 0;
}

void lcdIconRequest(const char* basename) {
    if (!basename || !*basename) return;
    if (lcdIconReady(basename)) { lcdLauncherIconLoaded(basename); return; }
    LoadReq req{};
    absPath(basename, req.path, sizeof(req.path));
    safeStrncpy(req.basename, basename, sizeof(req.basename));
    if (s_loadQ && xQueueSend(s_loadQ, &req, 0) != pdTRUE) warn("icon load queue full\n");
}

const char* lcdIconRes(void) { return s_res.c_str(); }

bool lcdIconResRefresh(void) {
    std::string nr = storageGetStr("s.lcd.icon_res", "40x40");
    if (nr.empty()) nr = "40x40";
    if (nr == s_res) return false;
    s_res = nr;
    return true;
}
