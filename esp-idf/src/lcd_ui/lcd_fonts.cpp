/**
 * lcd_fonts.cpp — the (face, px) → lv_font_t engine (see lcd_fonts.h).
 *
 * Three build variants behind one API (CONFIG_LCD_FONT_ENGINE):
 *
 *   FREETYPE  lv_freetype_font_create() over "/fixed/fonts/<face>.ttf" — plain
 *             stdio (FT_New_Face → fopen → IDF VFS → spanfs memcpy-from-mmap, so
 *             every read is safe from any task, PSRAM stacks included).
 *             Synthetic italic/bold via LV_FREETYPE_FONT_STYLE_* on the base face.
 *   TINY_TTF  the file bytes read once into a PSRAM buffer, handed to
 *             lv_tiny_ttf_create_data(). No hinting, no synthesis: the synthetic
 *             faces resolve to their base face.
 *   BITMAP    no engine — lcdFont() maps to the nearest compiled-in bitmap.
 *
 * Every created UI/MONO font gets its .fallback chained to the SYMBOLS face
 * (the vector FontAwesome subset symbols.ttf; stock lv_font_montserrat_14 in
 * BITMAP mode) so LV_SYMBOL_* renders unchanged. Fonts are cached per
 * (face, px); lcdFontsReset() frees them all.
 *
 * Lcd-task-only; no locks.
 */
#include "lcd_fonts.h"
#include "lcd_internal.h"      /* the compiled bitmap fonts */

#include "log.h"
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstdint>
#include <unordered_map>

namespace {

/* One shipped face → one TTF path. */
const char* kUiPath        = "/fixed/fonts/ui.ttf";
const char* kUiSemiPath    = "/fixed/fonts/ui-semibold.ttf";
const char* kMonoPath      = "/fixed/fonts/mono.ttf";
const char* kSymPath       = "/fixed/fonts/symbols.ttf";   /* FontAwesome subset */

struct Cached {
    const lv_font_t* font = nullptr;   /* what lcdFont() hands back            */
    uint8_t*         data = nullptr;   /* tiny_ttf byte buffer (freed on reset)*/
    bool             owned = false;    /* created by an engine → delete on reset*/
};

std::unordered_map<uint32_t, Cached> s_cache;   /* key = (face<<8)|px */

bool s_ftReady = false;

uint32_t keyOf(LcdFace face, int px) { return ((uint32_t)face << 8) | (uint32_t)(px & 0xFF); }

/* ---- compiled-bitmap fallbacks ---- */

const lv_font_t* bitmapUi(int px) {
    return px >= 15 ? &lv_font_montserrat_16_latin : &lv_font_montserrat_12_latin;
}
const lv_font_t* bitmapMono(int px) {
    if (px <= 3) return &lv_font_micro_2x3;
    if (px <= 6) return &lv_font_tomthumb_4x6;
    return &lv_font_spleen_5x8;
}
const lv_font_t* symbolFont() { return &lv_font_montserrat_14; }

/* The nearest bitmap for a face (BITMAP engine, or the sub-vector-px clamp). */
const lv_font_t* bitmapFor(LcdFace face, int px) {
    switch (face) {
        case LcdFace::SYMBOLS:    return symbolFont();
        case LcdFace::MONO:
        case LcdFace::MONO_BOLD:
        case LcdFace::MONO_ITALIC: return bitmapMono(px);
        default:                   return bitmapUi(px);   /* UI* */
    }
}

#if CONFIG_LCD_FONT_FREETYPE || CONFIG_LCD_FONT_TINY_TTF

/* Resolve a face to its backing file + FreeType style. Under tiny_ttf there is
 * no synthesis, so italic/bold-of-a-base collapse to the base file, NORMAL. */
void resolveFace(LcdFace face, const char*& path, lv_freetype_font_style_t& style) {
    style = LV_FREETYPE_FONT_STYLE_NORMAL;
    switch (face) {
        case LcdFace::UI:          path = kUiPath;     break;
        case LcdFace::UI_BOLD:     path = kUiSemiPath; break;   /* real SemiBold file */
        case LcdFace::UI_ITALIC:   path = kUiPath;
#if CONFIG_LCD_FONT_FREETYPE
                                   style = LV_FREETYPE_FONT_STYLE_ITALIC;
#endif
                                   break;
        case LcdFace::MONO:        path = kMonoPath;   break;
        case LcdFace::MONO_BOLD:   path = kMonoPath;
#if CONFIG_LCD_FONT_FREETYPE
                                   style = LV_FREETYPE_FONT_STYLE_BOLD;
#endif
                                   break;
        case LcdFace::MONO_ITALIC: path = kMonoPath;
#if CONFIG_LCD_FONT_FREETYPE
                                   style = LV_FREETYPE_FONT_STYLE_ITALIC;
#endif
                                   break;
        case LcdFace::SYMBOLS:     path = kSymPath;    break;   /* vector FontAwesome */
        default:                   path = kUiPath;     break;
    }
}

#if CONFIG_LCD_FONT_TINY_TTF
/* Read a whole /fixed file into a PSRAM buffer (tiny_ttf references it for the
 * font's lifetime, so we keep it and free it on reset). */
uint8_t* slurp(const char* path, size_t* outLen) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return nullptr; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return nullptr; }
    *outLen = (size_t)sz;
    return buf;
}
#endif

/* Create a vector font for (face, px). Chains the symbol fallback. Returns a
 * Cached (font may be null on failure — caller substitutes a bitmap). */
Cached createVector(LcdFace face, int px) {
    Cached c;
    const char* path = kUiPath;
    lv_freetype_font_style_t style;
    resolveFace(face, path, style);

#if CONFIG_LCD_FONT_FREETYPE
    lv_font_t* f = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                           (uint32_t)px, style);
    if (f) { c.font = f; c.owned = true; }
    else   warn("lcd_fonts: freetype create failed for %s @%dpx\n", path, px);
#elif CONFIG_LCD_FONT_TINY_TTF
    size_t len = 0;
    uint8_t* buf = slurp(path, &len);
    if (buf) {
        lv_font_t* f = lv_tiny_ttf_create_data(buf, len, px);
        if (f) { c.font = f; c.owned = true; c.data = buf; }
        else   { free(buf); }
    }
#endif

    /* Chain the symbol fallback onto the created font (UI/MONO only; SYMBOLS is
     * a leaf). LV_SYMBOL_* then resolves through the vector FontAwesome face at
     * the same px, so symbols scale with the text they sit in. */
    if (c.font && face != LcdFace::SYMBOLS)
        ((lv_font_t*)c.font)->fallback = lcdFont(LcdFace::SYMBOLS, px);
    return c;
}

#endif /* vector engines */

}  // namespace

void lcdFontsInit(void) {
    /* Nothing to do: lv_init() already brings FreeType up
     * (lv_init.c → lv_freetype_init(LV_FREETYPE_CACHE_FT_GLYPH_CNT)) when
     * LV_USE_FREETYPE=y, and tiny_ttf needs no library init. Kept as the
     * documented bring-up hook. */
    (void)s_ftReady;
}

const lv_font_t* lcdFont(LcdFace face, int px) {
    if (px < 4)   px = 4;
    if (px > 200) px = 200;

#if CONFIG_LCD_FONT_BITMAP
    /* No engine: SYMBOLS → montserrat_14 (FontAwesome merged), text → bitmaps. */
    return bitmapFor(face, px);
#else
    const bool isMono = (face == LcdFace::MONO || face == LcdFace::MONO_BOLD ||
                         face == LcdFace::MONO_ITALIC);
    const bool isUi   = (face == LcdFace::UI || face == LcdFace::UI_BOLD ||
                         face == LcdFace::UI_ITALIC);

    if (isMono) {
        /* Hand-tuned bitmap band for small terminal text — crisper than soft
         * antialiased vector at these sizes (LVGL's FreeType is always AA). The
         * two cells that exist: tomthumb 4×6, spleen 5×8. Anything bigger — or
         * smaller than tomthumb — renders as vector. */
        if (px >= 5 && px <= 6) return &lv_font_tomthumb_4x6;
        if (px >= 7 && px <= 8) return &lv_font_spleen_5x8;
    } else if (isUi) {
        /* 14 px is the smallest UI face — below that, proportional vector is too
         * soft to read, and we'd rather not shrink chrome further. */
        if (px < 14) px = 14;
    }

    /* General clamp: a bitmap floor for a whole image (0 = off by default; kept
     * so a board can dial one back in — e.g. a high-DPI panel zoomed way out). */
    if (px < CONFIG_LCD_FONT_MIN_VECTOR_PX) return bitmapFor(face, px);

    uint32_t key = keyOf(face, px);
    auto it = s_cache.find(key);
    if (it != s_cache.end() && it->second.font) return it->second.font;

    Cached c = createVector(face, px);
    if (!c.font) {
        /* Engine failure (missing file, OOM): use a bitmap, but don't cache the
         * failure — a later reset/retry may succeed. */
        return bitmapFor(face, px);
    }
    s_cache[key] = c;
    return c.font;
#endif
}

void lcdFontsReset(void) {
#if !CONFIG_LCD_FONT_BITMAP
    for (auto& kv : s_cache) {
        Cached& c = kv.second;
        if (c.owned && c.font) {
#if CONFIG_LCD_FONT_FREETYPE
            lv_freetype_font_delete((lv_font_t*)c.font);
#elif CONFIG_LCD_FONT_TINY_TTF
            lv_tiny_ttf_destroy((lv_font_t*)c.font);
#endif
        }
        if (c.data) free(c.data);
    }
    s_cache.clear();
#endif
}
