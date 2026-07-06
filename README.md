# spangap-lcd — the on-device LVGL UI

**spangap-lcd** is the on-device user interface for the [spangap](../spangap)
platform: a phone-style shell (status bar, paged launcher, navigation, recents
switcher) on top of a per-app object model, with built-in Settings, Log and CLI
apps, drawn with [LVGL](https://lvgl.io) v9 on an SPI display. It is the screen
counterpart to the browser SPA in [spangap-web](../spangap-web) — a separate UI
surface for devices that ship with a panel, not built on top of the web side.

The straddle owns the display, the input devices, and the one LVGL context. Any
straddle that wants a screen presence installs an app or a settings pane through
the public API; it never touches `lv_*` directly.

## Origins

The shell is a clean reimplementation of the phone system UI from
[ESP-Brookesia](https://github.com/espressif/esp-brookesia) — used as a
behavioural reference, not a dependency. No Brookesia type, macro, or name
survives; the app object model, the chrome, and the stylesheet-as-data approach
are reimplemented in spangap conventions (the `info/warn/err` log surface, the
lcd-task threading model, storage, `fs_*`, PSRAM-aware allocation, Kconfig).

The on-device terminal is backed by **libvterm** (the neovim mirror, MIT),
vendored under [`esp-idf/libvterm/`](esp-idf/libvterm). LVGL is pinned by this
straddle's own `idf_component.yml`, so consumers don't choose its version.

## The functions, and where they're documented

| Function | Operator guide | Maintainer reference |
|---|---|---|
| **Shell** — launcher, status bar, navigation, recents switcher, the foreground/back/home state machine | [docs/shell.md](docs/shell.md) | [docs/shell-internals.md](docs/shell-internals.md) |
| **Apps** — the `LcdApp` lifecycle, `lcdInstall`, input groups, per-app keypad focus | [docs/apps.md](docs/apps.md) | [docs/apps-internals.md](docs/apps-internals.md) |
| **Settings** — `lcdRegisterSettings` + the `lcdSetting*` pane builders | [docs/settings.md](docs/settings.md) | [docs/settings-internals.md](docs/settings-internals.md) |
| **Terminal** — the virtualized text view and the libvterm-backed VT100 terminal | [docs/terminal.md](docs/terminal.md) | [docs/terminal-internals.md](docs/terminal-internals.md) |

The shared foundation that every function sits on — the display bring-up, the
input HAL, auto-init, icons, fonts, and the display-power keys — is described
below; its maintainer detail is in
[docs/shell-internals.md](docs/shell-internals.md).

## How it starts, and how others plug in

When `spangap-lcd` is in the build, `lcdInit()` runs automatically: it is the
last entry in the platform band (core → net → web → lcd) of the generated
`spangapInitStraddles()` dispatcher, so the lcd task — and the display — are up
before any other straddle's init runs. There is no hand-written init call. A
straddle that paints a tile or a settings pane is therefore guaranteed the lcd
task already exists.

Presence of the straddle is the on/off switch. A consumer lists
`spangap/spangap-lcd` under `additional_installs:` in its `straddle.yaml` (so a
`spangap build --no-lcd` can leave it out); when it is staged, `spangap-inside`
emits the presence symbol `CONFIG_SPANGAP_LCD`, and the consumer guards its UI
code on it:

```cpp
#if CONFIG_SPANGAP_LCD
    lcdInstall(new MyApp());                       // a launcher app (lcd_app.h)
    lcdRegisterSettings("Net/Wifi", "Wifi", buildPane);   // a settings pane (lcd.h)
#endif
```

Both calls run from the consumer's own init hook. `lcdInstall` and
`lcdRegisterSettings` must run on the lcd task — register from an init that the
build already routes there, or hop on with `lcdRun()`/`ON_LCD`. See
[docs/apps.md](docs/apps.md) and [docs/settings.md](docs/settings.md).

## The display

The display is the component's own concern: an SPI panel (bus, controller,
backlight, orientation) configured entirely through Kconfig (`CONFIG_LCD_*`) and
brought up by `lcd_panel.cpp`. A board with a standard SPI panel contributes no
display code — it sets the pins in its `sdkconfig.defaults` and supplies only
the input HAL (below).

| Kconfig | Default | Meaning |
|---|---|---|
| `LCD_SPI_HOST` | `2` | SPI peripheral (1=SPI1, 2=SPI2/FSPI, 3=SPI3); shares the bus with SD/LoRa via `spi_helper`. |
| `LCD_SCK_PIN` / `LCD_MOSI_PIN` / `LCD_MISO_PIN` | `-1` | Shared-bus pins (MISO `-1` if unused). |
| `LCD_CS_PIN` / `LCD_DC_PIN` / `LCD_RST_PIN` | `-1` | Panel chip-select / data-command / reset (`-1` if reset rides the power rail). |
| `LCD_BL_PIN` | `-1` | Backlight pin, driven as LEDC PWM (`-1` if not host-controlled). |
| `LCD_PCLK_MHZ` | `40` | Pixel clock; the GPIO matrix caps the ESP32-S3 near 40 MHz. |
| `LCD_CONTROLLER_ST7789` / `LCD_CONTROLLER_ILI9341` | ST7789 | Panel controller. ST7789 is built into `esp_lcd`; ILI9341 pulls in `esp_lcd_ili9341`. |
| `LCD_NATIVE_WIDTH` / `LCD_NATIVE_HEIGHT` | `240` / `320` | Native pixels, pre-rotation (an SPI panel can't report its glass size). |
| `LCD_ROTATION` | `90` | Hardware rotation (0/90/180/270); applied as swap_xy+mirror, same transform applied to raw touch. |
| `LCD_MIRROR_X` / `LCD_MIRROR_Y` | `n` | Correct a mirrored image when the panel's scan direction differs. |
| `LCD_INVERT_COLOR` | `y` | Most ST7789 IPS panels need inversion. |
| `LCD_SETTINGS_MARQUEE` | `y` | In Settings, a long read-only value scrolls horizontally on keypad focus instead of wrapping (see [docs/settings.md](docs/settings.md)). |

## The input HAL

What stays board-specific is input. A board fills an `lcd_input_t`
([lcd_input.h](esp-idf/include/lcd_input.h)) and registers it with
`lcdSetInput()` **before** `spangapInit()`. Every member is optional — a board
may register nothing and the UI still comes up, navigable by a keypad/keyboard
indev the board joins to `lcdInputGroup()`.

- `init` — one-time setup on the lcd task (wire INT lines, create touch handles).
- `touch_read(pts, max, *count)` — raw native touch points (re-polled ~10 ms
  while a finger is down); the component applies the panel's rotation/mirror.
- `pointer_read(*x, *y)` — an absolute cursor device (trackball / mouse); the
  board owns the position (integrates motion, clamps to `lcdDisplaySize()`), the
  component draws an auto-hiding cursor.
- `click_read()` — the centre/Home button; the board owns all timing and calls
  `lcdGoHome()` itself on a long-press.

The board attaches the component's exported `lcdInputISR` to its input INT lines
(`gpio_isr_handler_add`); it only flags and wakes the lcd task, which reads its
event-mode indevs on demand (no input polling). A hardware text keyboard is not
part of the HAL: a board with one creates its own keypad indev on
`lcdInputGroup()` and calls `lcdSetHasKeyboard(true)`. For the LilyGo T-Deck
Plus, [hw-tdeck](../hw-tdeck) provides the input HAL (GT911 touch, trackball,
centre button) and sets the panel pins.

A board that reads its own keys (the built-in touch/button/trackball indevs
report their own activity) tells the inactivity tracker about them with
`lcdNotifyActivity()` — it resets the blank timer, wakes the screen if it is in
standby, and returns `true` iff that call did the waking, so an on-lcd-task
caller can swallow the waking keystroke. Two more board-facing knobs round out
the cursor:

- `lcdPointerSetVisibleMs(ms)` — how long the cursor stays drawn after pointer
  activity (`<0` = always visible). The component owns the cursor; the board
  that owns the pointing device sets the policy (e.g. from its own config key).
- `lcdScrollwheelArrowsActive()` — true while the foreground app has requested
  trackball arrow-mode (an app's `setScrollwheelArrows()`; see
  [docs/apps.md](docs/apps.md)). A trackball board's `pointer_read` consults it
  and feeds arrow keys into `lcdInputGroup()` instead of moving the pointer, so a
  terminal / vim can navigate.

**Multi-touch / gestures.** Single-touch is the default (the cheapest path). A
board that wants raw multi-finger input (e.g. pinch-zoom) enables multipoint
reads with `lcdTouchSetMultipoint(true)` and registers a handler with
`lcdTouchAddGestureHandler(cb)` (a fixed small set, called once at init). The
handler runs on the lcd task with the current `lcd_touch_pt_t` points (display
coordinates) and finger `count` whenever touch is sampled; the high-rate finger
data never goes through storage. Point 0 still drives the normal single-pointer
indev, but with ≥2 fingers down the pointer is suppressed so the gesture owner
has the interaction. No-op on a board whose `touch_read` reports a single point.

**Edge-pan on a cursor-only board.** A touchless trackball deck can't reach
offscreen content by moving the pointer once it's clamped at an edge, so the
board's `pointer_read` calls `lcdScroll(dir, amount)` with the step the clamp
would otherwise swallow; the shell pans whatever is shown (a scroll-container
app or the launcher). Touchscreen boards never need it. App authors with
non-scroll content register their own pan handler — see [docs/apps.md](docs/apps.md).

## Display power: backlight, inactivity, standby

`lcdSetBacklight(level)` (0..255) writes `s.lcd.backlight`; a subscription
applies it on the lcd task, so writing the key directly (browser, CLI) has the
identical effect. After `s.lcd.inactivity_timeout` seconds with no input the
component does **not** blank itself — it only sets the ephemeral `sys.standby`
key. **Standby is the board's policy:** the board subscribes to `sys.standby`
and calls `lcdScreenSleep()` / `lcdScreenWake()` (backlight off + panel display
off, GRAM retained for an instant fade-in wake) and powers its own input
down/up. The board's button sets/clears the same key, so timeout and button
share one path.

## Storage variables

All keys are owned by this component. `s.*` settings sync to the browser.

| Key | Default | Meaning |
|---|---|---|
| `s.lcd.backlight` | `200` | Backlight 0..255 (0 = off); applied live. |
| `s.lcd.inactivity_timeout` | `30` | Seconds of no input before `sys.standby` is set; `0` = never. |
| `s.lcd.date_format` | `"%d %b %Y, %H:%M"` | `strftime` format for the status-bar clock (live). |
| `sys.standby` | — | Ephemeral. Set by the component on inactivity, set/cleared by the board's button; the board acts on it. |

The status bar also *reads* keys owned by other straddles to render its glyphs:
`wifi.sta.state` / `wifi.sta.rssi` / `wifi.sta.up` (net), `battery.percent`
(board), `sys.time.valid` (core). The Log app reads `s.log.file.paste` (core
logging) to size its scrollback. None of these are owned here.

## Icons

Launcher icons are LVGL `RGB565A8` `.bin` files, shipped read-only under
`/fixed/lcd/icons/36x36/<name>.bin` and loaded by basename. Drop `*.svg` /
`*.png` sources outside `data/` and rasterize them at build time with the
`spangap_lcd_icons()` CMake helper
([project_include.cmake](esp-idf/project_include.cmake)):

```cmake
spangap_lcd_icons(SRC_DIR "${CMAKE_SOURCE_DIR}/assets/lcd-icons" SIZES "36x36")
```

It always also rasterizes this straddle's own [`assets/lcd-icons/`](esp-idf/assets/lcd-icons)
(`gear`, `log`, `cli` — the built-in apps' icons), merged by basename with the
consumer winning on a collision, so `SRC_DIR` is optional. It is best-effort: if
the host lacks Pillow / cairosvg / `LVGLImage.py` it warns and skips, the build
still succeeds, and tiles render label-only until icons are present. You may also
drop pre-rendered `.bin` files under `<consumer>/data/lcd/icons/36x36/`.

## Fonts

Beyond LVGL's stock set, the straddle ships checked-in C font arrays (declared
in [lcd.h](esp-idf/include/lcd.h)); set any on your own widgets with
`lv_obj_set_style_text_font()`:

- **`lv_font_montserrat_12_latin`** / **`lv_font_montserrat_16_latin`** —
  accented supersets of LVGL's Montserrat (Latin-1 Supplement + Latin Extended-A),
  so user text renders accents instead of placeholder boxes. The 12px keeps the
  full symbol set; the chrome font.
- **`lv_font_spleen_5x8`** — the terminal font (2 bpp), with the complete Box
  Drawing block and synthesized Block Elements for column-aligned content.
- **`lv_font_tomthumb_4x6`** — the smallest monospace (80 columns at 320 px),
  even-width so 2×2 block glyphs tile seamlessly (micron/NomadNet page graphics).
- **`lv_font_micro_2x3`** — a deliberately unreadable page-thumbnail font (160
  columns), the 4×6 set box-filtered 2× down.

Regenerate any of them with the matching `scripts/gen-*.py` and commit the `.c`;
fonts are checked in so an ordinary build needs no tooling. Maintainer detail
(generation, box-drawing synthesis, licensing) is in
[docs/terminal-internals.md](docs/terminal-internals.md).

## Dependencies

- [spangap-core](../spangap-core) — base runtime (ITS, storage, log, CLI, fs, mem).
- `lvgl`, `esp_lcd` (+ `esp_lcd_ili9341` when selected) — linked by this straddle,
  not pushed onto consumers (except `lvgl`, which `lcd.h` includes).

## What it does NOT own

- The touch/cursor/button hardware reads — the consuming board's input HAL.
- The browser SPA — [spangap-web](../spangap-web).
- Per-app UI content — each app/pane is owned by the straddle that installs it.
