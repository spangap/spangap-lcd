# spangap-lcd

## What is this?

**spangap-lcd** is the on-device LVGL UI for the [spangap](../spangap)
platform: an LCD launcher, status bar, a built-in Settings menu, built-
in Log and CLI programs, plus the activator that folds every other
straddle's on-device UI slice into one image.

Parallel to (not built atop) `spangap-web` — the LCD launcher is its own
UI surface for devices that ship with a screen, separate from the
browser SPA.

## What this straddle owns

```
spangap-lcd/
└── esp-idf/
    ├── include/
    │   ├── lcd.h          public API (lcdRegister, lcdRegisterSettings)
    │   └── lcd_input.h    input-HAL interface — consumer provides touch/cursor/button
    ├── src/lcd_ui/
    │   ├── lcd.cpp        LVGL context, the LVGL task
    │   ├── lcd_panel.cpp        SPI panel bring-up from Kconfig (ST7789/ILI9341)
    │   ├── lcd_launcher.cpp     program-tile grid
    │   ├── lcd_statusbar.cpp    top bar (WiFi/upstream/time/battery)
    │   ├── lcd_settings.cpp     built-in Settings menu + lcdSetting* helpers
    │   ├── lcd_apps.cpp         built-in Log + CLI program tiles
    │   ├── lcd_textview.cpp     terminal view used by Log/CLI
    │   ├── lcd_icons.cpp        icon binding
    │   ├── lcd_lvgl.cpp         LVGL glue
    │   ├── lv_font_spleen_5x8.c       Spleen 5×8 monospace, 2 bpp (Log/CLI; full
    │   │                              box-drawing/block-element "terminal graphics")
    │   ├── lv_font_tomthumb_4x6.c     Tom Thumb 4×6 monospace (smallest readable;
    │   │                              full box-drawing/block-element "terminal graphics")
    │   ├── lv_font_micro_2x3.c        Micro 2×3, 2 bpp — unreadable page THUMBNAIL
    │   │                              (the 4×6 set box-filtered 2×; 160 columns)
    │   └── lv_font_montserrat_12_latin.c   Montserrat 12 latin (chrome)
    ├── assets/lcd-icons/        cli.svg, log.svg, gear.svg (rasterized at build)
    └── scripts/
        ├── lcd-icons.py         SVG→RGB565 launcher tiles
        ├── gen-spleen-font.py
        ├── gen-tomthumb-font.py
        ├── gen-micro-font.py
        ├── gen-text-font.py
        ├── spleen-5x8.bdf
        ├── tom-thumb.bdf        Tom Thumb (MIT) — 4×6 text glyphs
        ├── fixed-4x6.bdf        X11 misc-fixed 4×6 (public domain) — graphics glyphs
        └── fixed-5x8.bdf        X11 misc-fixed 5×8 (public domain) — graphics glyphs
```

## How others use it — programs

Any straddle that wants an on-device UI lists `spangap/spangap-lcd`
under its `additional_installs:` in `straddle.yaml`, then calls
`lcdRegister` from its `init()` gated on `CONFIG_SPANGAP_LCD` — the
short-form alias that `spangap-inside` emits whenever this straddle
is in the staged set (a `--no-lcd` build leaves it undefined):

```cpp
#if CONFIG_SPANGAP_LCD
    lcdRegister("My App", "myicon", myAppRunFn);
#endif
```

Settings panes register via:

```cpp
#if CONFIG_SPANGAP_LCD
    lcdRegisterSettings("Group/Item", "Item", buildPaneFn);
#endif
```

Build the pane with the `lcdSetting*` helpers (the on-device cousin of
the browser's `Setting*` components — config-bound).

**Scrolling on a cursor-only board.** On a touchless, trackball-only
deck the user reaches offscreen content by driving the pointer into a
screen edge; the board reports that as `lcdScroll()`, which pans the
shown program's main scroll area (or the launcher) — so any program
built from ordinary LVGL scroll containers (a flex column, a text view)
just works, nothing to do. A program whose content is **not** an LVGL
scroll container — e.g. a canvas it repositions itself — registers a pan
handler from its run fn instead and folds the delta into its own pan:

```cpp
#if CONFIG_SPANGAP_LCD
    lcdProgramScrollHandler(myPanFn);   // void myPanFn(int dx, int dy)
#endif
```

The `(dx, dy)` use the finger-drag sign convention (the same vector a
touch drag yields), and the hook is active only while that program is the
one on screen. (Unrelated: `lcdProgramScrollwheelArrows(true)` instead
diverts the whole trackball to arrow keys, for an on-device terminal.)

## How others use it — activator

`spangap-lcd` is **the LCD-side UI activator**. When this straddle is in
the dep graph (and `--no-lcd` is not set), the build:

1. Walks every other straddle's `esp-idf/lcd/` slice.
2. Folds that slice into the *owning straddle's* firmware component
   (the slice's `.cpp` includes use the straddle's existing
   `#include` surface plus the auto-generated `CONFIG_STRADDLE_*`
   presence symbols).
3. Calls the slice's `<prefix>LcdInit()` from the generated dispatcher.

The slice's `.cpp` body is typically wrapped in
`#if CONFIG_SPANGAP_LCD` so straddles compile without it as well.
`CONFIG_SPANGAP_LCD` is auto-emitted as a short-form alias by
`spangap-inside` when this straddle is staged — no user-facing
`menuconfig` knob. See `spangap/INTERNALS.md` for the
`additional_installs:` mechanism that drives it.

A slice for straddle `foo` lives in `foo/esp-idf/lcd/src/foo_lcd.cpp`
and registers programs / panes via the public API above.

## What the consumer must supply

The input HAL (`lcd_input.h`) — touch, a cursor device (trackball/mouse), and a
centre/Home button — implemented by the consuming buildable straddle and
registered with `lcdSetInput()`. All members are optional (a keyboard-only board
registers nothing). The **display** is not the consumer's concern: the SPI panel
(bus, ST7789/ILI9341 controller, backlight, orientation) is configured through
Kconfig (`CONFIG_LCD_*`) and brought up by `lcd_panel.cpp`. For the LilyGo
T-Deck Plus, the hw-tdeck straddle provides the input HAL in `tdeck.cpp` and sets
the panel pins in its `sdkconfig.defaults`.

## What it does NOT own

- The touch/cursor/button hardware reads — those live in the consuming
  buildable straddle's input HAL.
- The browser SPA — that's [spangap-web](../spangap-web).
- Per-app UI content — those are slices in each straddle's
  `esp-idf/lcd/` subdir, owned by the straddle that registered the
  program / setting.

## Read next

- [INTERNALS.md](INTERNALS.md) — LVGL task model, icon pipeline, the
  Log/CLI terminal pattern, font generation.
- For the on-device UX deep dive: see
  [spangap-core/docs/lcd.md](../spangap-core/docs/lcd.md) (this is
  scheduled to migrate here).
