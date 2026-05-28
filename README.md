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
    │   └── lcd_board.h    board-HAL interface — consumer provides the impl
    ├── src/lcd_ui/
    │   ├── lcd.cpp        LVGL context, the LVGL task
    │   ├── lcd_launcher.cpp     program-tile grid
    │   ├── lcd_statusbar.cpp    top bar (WiFi/upstream/time/battery)
    │   ├── lcd_settings.cpp     built-in Settings menu + lcdSetting* helpers
    │   ├── lcd_apps.cpp         built-in Log + CLI program tiles
    │   ├── lcd_textview.cpp     terminal view used by Log/CLI
    │   ├── lcd_icons.cpp        icon binding
    │   ├── lcd_lvgl.cpp         LVGL glue
    │   ├── lv_font_spleen_5x8.c       Spleen 5×8 monospace (Log/CLI)
    │   └── lv_font_montserrat_12_latin.c   Montserrat 12 latin (chrome)
    ├── assets/lcd-icons/        cli.svg, log.svg, gear.svg (rasterized at build)
    └── scripts/
        ├── lcd-icons.py         SVG→RGB565 launcher tiles
        ├── gen-spleen-font.py
        ├── gen-text-font.py
        └── spleen-5x8.bdf
```

## How others use it — programs

Any straddle that wants an on-device UI calls `lcdRegister` from its
`init()` (gated on `CONFIG_SPANGAP_LCD`):

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

## How others use it — activator

`spangap-lcd` is **the LCD-side UI activator**. When this straddle is in
the dep graph (and `--no-lcd` is not set), the build:

1. Walks every other straddle's `esp-idf/lcd/` slice.
2. Folds that slice into the *owning straddle's* firmware component
   (the slice's `.cpp` includes use the straddle's existing
   `#include` and Kconfig surface).
3. Calls the slice's `<prefix>LcdInit()` from the generated dispatcher.

The slice's `.cpp` body is typically wrapped in
`#if CONFIG_SPANGAP_LCD` so straddles compile without it as well.

A slice for straddle `foo` lives in `foo/esp-idf/lcd/src/foo_lcd.cpp`
and registers programs / panes via the public API above.

## What the consumer must supply

The board HAL (`lcd_board.h`) — the consuming buildable straddle implements
it. Display panel init, touch HAL, keyboard / trackball / button input,
backlight control, dpi/orientation. For the LilyGo T-Deck Plus, the
reticulous-tdeck straddle provides this in `tdeck.cpp`.

## What it does NOT own

- The display/touch hardware drivers — those live in the consuming
  buildable straddle's board HAL.
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
