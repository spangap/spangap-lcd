# spangap-lcd — internals

## LVGL context

`lcd.cpp` owns the LVGL context and the LVGL task. The display, the
touch, and any other input device feed into this one task — there is no
second LVGL context anywhere in the system. Tasks that want to drive
the UI do so by registering programs / settings panes; they do NOT
touch `lv_*` directly.

The launcher tile grid (`lcd_launcher.cpp`), the status bar
(`lcd_statusbar.cpp`), and the built-in Settings menu
(`lcd_settings.cpp`) are LVGL widgets the lcd task paints. The built-in
Log + CLI programs (`lcd_apps.cpp` + `lcd_textview.cpp`) act as ITS
clients to `log:1` / `cli:1` and stream into a Spleen 5×8 terminal
view.

## Activator: folding LCD slices into straddles

A straddle's `esp-idf/lcd/` subdir is the slice the LCD activator folds
in. Folding means:

- The slice's `.cpp` files are added to the *owning straddle's*
  component sources at CMake configure time.
- The slice's `#include` surface is the same as the rest of that
  straddle's component (it sees the straddle's own internal headers).
- A `<prefix>LcdInit()` symbol exported by the slice is called from the
  generated dispatcher.

This is why the slice's body is conventionally wrapped in
`#if CONFIG_SPANGAP_LCD` — without `spangap-lcd` in the staged set the
symbol is undefined, so the slice is still in the source tree (going
through git/IDE correctly) but compiles to nothing. The
`CONFIG_SPANGAP_LCD` symbol is the short-form alias `spangap-inside`
emits when `spangap-lcd` is staged; the long-form generic equivalent
is `CONFIG_STRADDLE_SPANGAP_LCD`. See `../spangap/INTERNALS.md` for
the `additional_installs:` mechanism that drives both.

The folding is "hardcoded" in the firmware CMakeLists.txt of each
slice's straddle for now — the activator-driven source-list exclusion is
a future build-CLI feature.

## Public API surface

```c
// lcd.h (high-level)
void lcdRegister(const char* name, const char* iconKey, lcdProgramFn fn);
void lcdRegisterSettings(const char* path, const char* label, lcdPaneFn fn);

// lcdSetting* helpers — config-bound widgets matching the browser's SettingX components
lv_obj_t* lcdSettingToggle(lv_obj_t* parent, const char* key, const char* label);
lv_obj_t* lcdSettingSlider(lv_obj_t* parent, const char* key, const char* label, int min, int max);
// ...
```

The full set is in `include/lcd.h`.

## Icon pipeline

`scripts/lcd-icons.py` walks `assets/lcd-icons/*.svg`, rasterizes via
cairosvg + Pillow, and emits RGB565 + alpha tile data into a header
embedded at build time. Depends on `cairo + Pillow + cairosvg + pypng +
lz4` — without all four it silently ships label-only tiles (the
launcher will show text where icons should be).

Adding a custom icon: drop SVG into the consuming app's
`assets/lcd-icons/` (consumer's assets merge with this straddle's at
build time) and reference its key in `lcdRegister(name, key, fn)`.

## Fonts

- **Spleen 5×8** — `lv_font_spleen_5x8.c` generated from
  `scripts/spleen-5x8.bdf` by `gen-spleen-font.py`. The terminal font.
- **Montserrat 12 latin** — `lv_font_montserrat_12_latin.c` generated
  by `gen-text-font.py`. The chrome font (titles, menus).

If you change a font, re-run the generator and commit the resulting
`.c`.

## Why this is its own straddle

Phase-2 split from the old monolithic `spangap-core`. The previous
arrangement compiled the LVGL launcher unconditionally; the screen-less
seccam build had to `#if` it out everywhere. As a separate straddle
the LCD UI is opt-in by dependency — present a screen, depend on
`spangap-lcd`; ship headless, don't.

## Display config + consumer-supplied input HAL

The display is the component's own: `lcd_panel.cpp` brings up the SPI bus, the
controller (ST7789 built into esp_lcd, or ILI9341 via the managed component), the
LEDC backlight, and the orientation — all from Kconfig (`CONFIG_LCD_*`, see
`Kconfig`). Resolution can't be probed from an SPI panel, so native size +
rotation are config; the same rotation is applied to raw touch.

`include/lcd_input.h` declares the only thing the consuming app implements: the
input HAL — `touch_read` (raw native points), `pointer_read` (cursor device),
and `click_read` (centre/Home button; the board owns the click-vs-hold policy and
calls `lcdGoHome()` on a hold). All optional. The hw-tdeck straddle provides this
in `tdeck.cpp` for the LilyGo T-Deck Plus (GT911 touch, trackball, centre button).
