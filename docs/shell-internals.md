# Shell — internals

Maintainer reference for the phone shell (`src/lcd_ui/shell/`) and the display /
task foundation it sits on (`src/lcd_ui/`). The [operator guide](shell.md) is the
chrome view; this file is for changing it without breaking it. Everything here
runs on the lcd task unless noted.

## 1. What the shell adds

The shell is `src/lcd_ui/shell/`, glued by `shell_internal.h`, on top of the
display/LVGL/task foundation in `src/lcd_ui/`:

- **manager.cpp** — the foreground/back/home state machine, the home-bar drag
  chrome, and the retained free-function surface (`lcdShowProgram`, `lcdGoHome`/
  `lcdGoHomeInternal`, `lcdProgramFullscreen`, `lcdProgramScrollwheelArrows`,
  `lcdProgramScrollHandler`, `lcdScroll`, `lcdAtLauncher`) that bridges
  unconverted callers to the foreground app.
- **launcher.cpp** — the paged icon grid (a horizontal pager of flex-wrap pages
  + a dot row), `shellLauncherAddTile`, and the icon-loaded hook.
- **statusbar.cpp** — the opaque top bar renderer (clock/wifi/upstream/battery),
  all event-driven off storage subscriptions.
- **nav.cpp** — the ESC→Back producer. LVGL has no group-wide key hook, so it
  tracks the focused object via the group's focus callback and keeps an
  `LV_KEY_ESC` handler on whatever holds focus. ESC is passed *through* (not
  consumed as Back) when the foreground app owns raw keys — a terminal/arrow-mode
  app (`_arrows()`) or a focused textarea — so the gesture only means Back where
  it would otherwise be inert.
- **recents.cpp** — the app switcher (cards over the running set + a heap readout).
- **stylesheet.{h,cpp}** + **stylesheet_320x240.cpp** — theme/geometry as data.
- **lcd_app.cpp** — the `LcdApp` install registry and service methods (covered in
  [apps-internals.md](apps-internals.md)).

Nothing on screen is special-cased chrome: even Settings is just another installed
`LcdApp`. `lcd_settings.cpp`'s `lcdSettingsInit()` wraps the existing settings
page-stack in a thin `SettingsApp` host and `lcdInstall`s it (gear icon,
`launcherPage` 0), so it lifts, backgrounds, and appears in recents like any app.
The built-in Log and CLI terminals are likewise `LcdApp` subclasses under
`src/lcd_ui/apps/`. All of this compiles only when `CONFIG_LCD_PHONE=y`, which
makes the shell the single UI: the standalone `lcd_launcher.cpp` / `lcd_apps.cpp`
/ `lcd_statusbar.cpp` are gone, and the legacy free-function surface they exported
(`lcdRegister` and friends, listed in the manager bullet above) now lives *inside*
the shell as a bridge so unconverted straddles still link.

## 2. The lcd task & threading (foundation)

`lcd.cpp` spawns the **lcd task** ("lcd", prio 2, core 1 — core 0 hosts Wi-Fi —
16 KB **PSRAM** stack). PSRAM stack is safe because the task **never does flash
I/O**: it runs the canonical `itsPoll` loop whose notification accounting must
stay intact, so a blocking FS round-trip would desync it. Icon bytes are read by
a separate loader task (§7) and handed over with `lcdRun()`.

**LVGL is single-threaded; everything that touches `lv_*` runs on the lcd task.**
To run there from any other task, hop on with `lcdRun(fn, arg)` / `ON_LCD` (the
public sugar, mirroring storage's `ON_CHANGE`); it queues the call as an ITS aux
message on `LCD_RUN_PORT` (10) and returns immediately. `lcdGoHome()` is just
`lcdRun([]{ lcdGoHomeInternal(); })`; `lcdSetBacklight()` is just
`storageSet("s.lcd.backlight", …)` and the subscription does the rest.

The loop is event-driven: indevs are `LV_INDEV_MODE_EVENT`, so `lv_timer_handler`'s
idle return is honest and the task sleeps until the next LVGL timer or a wake (an
input ISR or an ITS message). `lcdInputISR` (IRAM, DRAM-only) only flags
`s_inputPending` + `vTaskNotifyGiveFromISR`s the task; the loop then drains
indevs via `lcdInputPoll()`. Three subtleties are load-bearing:

- **Released-indev timer pause.** LVGL resumes a pointer's read timer on press
  for its own long-press timing and can miss the release-pause, leaving it
  auto-reading at ~30 Hz; `lcdPauseIdleInputTimers()` runs each pass to stop it.
- **Blank skip.** While `lcdScreenIsOff()`, the loop skips rendering and sleeps
  on `portMAX_DELAY` so the chip can light-sleep; keyboard keys arrive as
  `lcdRun` aux and wake it.
- **Anim CPU boost.** Animation frames are timer wakes, not notifies, so the loop
  pins 240 MHz (`pmBoost`) on the rising edge of `lv_anim_count_running() > 0`
  and releases on the falling edge — otherwise inertial flings run at the DFS
  floor.

**Bring-up handshake.** `lcdInit()` spawns the task and **blocks on a binary
semaphore** until the task signals the panel controller is initialised and the
UI tree built (or a 5 s timeout). The panel shares the SPI bus with peripherals
whose init runs after `lcdInit()` returns (SD, LoRa), so the one-shot controller
sequence must be off the bus first. The task signals **before** the first frame
and lets the loop's `lv_timer_handler()` flush it — an inline `lv_refr_now()`
here would flush off the loop path and desync the `itsPoll` notification
accounting.

## 3. Display & LVGL bring-up (foundation)

`lcd_panel.cpp` brings up the SPI bus, panel-IO, and controller (ST7789 built
into `esp_lcd`, or ILI9341 via the managed component) entirely from
`CONFIG_LCD_*`; resolution can't be probed from an SPI panel, so native size +
rotation are config. The same rotation/mirror transform is applied to raw touch
(`lcdPanelOrientTouch`) so touch and pixels always agree. It also exposes the
LEDC backlight (`lcdPanelBacklight`) and panel display on/off for standby
(`lcdPanelDisplayPower`, GRAM retained for instant wake).

`lcd_lvgl.cpp` is LVGL v9 over `esp_lcd`:

- **Draw buffers** — two strips, double-buffered, in internal DMA-capable RAM
  (`MALLOC_CAP_DMA`), `RENDER_MODE_PARTIAL`; the strip line count fits the
  shared-bus `max_transfer_sz`.
- **Flush** — swaps RGB565 to big-endian (ST7789 wants BE) and **holds the
  shared-bus lock across the whole transfer including the async DMA drain**. This
  is load-bearing: `esp_lcd` releases the SPI driver's own lock the moment the
  colour DMA is *queued*, so without `spiHelperBusLock()` a co-resident polling
  driver (LoRa on the same SPI2 bus) can grab the bus mid-DMA and panic. The
  flush blocks on a binary semaphore given from the `on_color_trans_done` ISR.
- **Indevs** — touch (`LV_INDEV_TYPE_POINTER`, multipoint optional), a keypad
  indev for the board button, and a pointer-with-cursor indev for a trackball.
  A focus group (`lcdInputGroup()`) is created regardless so a button/keyboard-
  only board drives the same UI.

LVGL's heap is routed to spangap's central allocator: `lv_mem_spangap.cpp`
(gated on `CONFIG_LV_USE_CUSTOM_MALLOC`) implements LVGL's full custom-malloc
backend, routing `lv_malloc/realloc/free_core` through `gp_*` (PSRAM on PSRAM
targets). The straddle's `kconfig:` block adds `CONFIG_LV_USE_SNAPSHOT=y`, which
the recents thumbnails need (§6).

## 4. The state machine (manager.cpp)

Stack-free model: the launcher is the root, apps are peers, one app foreground at
a time. The **running set** (recents membership) is exactly the apps whose root
layer currently exists — `onCreate` called, `onClose`/eviction not yet — so the
resident layer *is* the membership, no separate bookkeeping. A program layer's
`user_data` holds its owning `LcdApp*` (non-null distinguishes a program layer
from the launcher grid), so a layer `LV_EVENT_DELETE` can null the app's root and
clear the foreground without any stored back-pointer to dangle.

`shellOpenApp(app)` sets the foreground **before** `onCreate` (so an app's
`setFullscreen`/`setScrollwheelArrows` during build bind to itself), builds the
layer lazily on first open, snapshots and hides the previous app, applies chrome,
restores focus, and hides the launcher behind the new app.

`shellNavigate(NavIntent)` is the single consumer:

- **HOME** — snapshot the foreground for recents, fade the launcher in over
  200 ms, slide the app up off the top (220 ms ease-in), then re-park it hidden
  and fire `onHide()`.
- **BACK** — `onBack()`; if unhandled, recurse to HOME.
- **RECENTS** — fade the foreground app out while `shellRecentsShow()` fades the
  switcher in; the app keeps running underneath, BACK restores it.

When recents is visible, a non-RECENTS intent first dismisses recents back to the
underlying app (restoring its opacity) or the launcher.

**Chrome.** `applyChrome()` derives status-bar visibility (hidden if the
foreground app is fullscreen or opts out via `Config::statusBar`), the foreground
layer's geometry (`y = statusBar.h`, or 0 fullscreen), and trackball-arrows mode
from the foreground app's flags. Re-run on every open/home and on `shellAppChanged`.

**Home-bar drag** is manual (LVGL gestures are unreliable on a short strip): a
centre-only clickable patch on `lv_layer_top()` with `PRESS_LOCK`, driving the
lifted-app translate by hand; release computes the lifted fraction and routes to
HOME / RECENTS / spring-back.

## 5. Per-app keypad focus

The launcher tiles, app widgets, and built-in nav all share one focus group
(`lcdInputGroup()`). To stop a backgrounded app from stranding the shared group's
focus on an offscreen widget, the manager **saves and restores focus per app**:
on leaving the foreground, `saveFocus(app)` records the focused object **iff it
belongs to that app's tree**; `restoreFocus(app)` re-focuses it on return (if
still valid). The saved focus is cleared when the app's root is deleted. App
authors add their own focusable widgets to `inputGroup()` and otherwise do
nothing — the save/restore is automatic (this is what `LcdApp::inputGroup()`
fronts). One rule for them: don't focus a widget at build time (`onCreate`)
unless the app should seize the keyboard the instant it opens — focus on tap
instead. Auto-focusing on build hands the shared keypad group to that app the
moment it is installed, which is the same failure the per-app save/restore
guards against from the other side.

## 6. Recents thumbnails (recents.cpp + lcd_app.cpp)

A recents card shows a half-scale PSRAM snapshot of the app's last foreground
frame. The manager calls `app->_captureThumb()` the moment the app leaves the
foreground (HOME slide-up, RECENTS fade, or being hidden by another open) — while
the root is still drawn and opaque. `_captureThumb` (`LcdApp`) uses
`lv_snapshot_take` (RGB565, matching the panel) into a PSRAM buffer, then
downscales to half size through a hidden canvas (`LV_USE_SNAPSHOT` + `LV_USE_CANVAS`),
keeping only the miniature; it falls back to the full snapshot if the canvas/mini
alloc fails. The thumbnail is freed when the app is evicted. Per-card vertical
drag mirrors the home-bar drag; total finger travel (`kTapSlop`) separates a tap
(open) from a horizontal scroll (between cards) from a vertical swipe (terminate).

`LcdApp::setRecentsSubtitle()` stores a per-app subtitle string, but `addCard`
currently renders only the name — the subtitle is not yet shown.

## 7. Icon cache + loader (lcd_icons.cpp, foundation)

Three-stage hand-off, no locks, solving "the lcd task can't do flash I/O":

1. **Loader task** ("lcd_load", prio 1, core 1, 4 KB PSRAM stack) blocks on a
   FreeRTOS request queue (`LCD_ICON_QUEUE_DEPTH` = 16). On a request it `fs_stat`s,
   rejects bad sizes (> `LCD_ICON_MAX_BYTES` = 256 KB), reads into a PSRAM buffer,
   and hands it back via `lcdRun(onLoaded, …)`. It has no `itsPoll` loop, so the
   fs proxy's pickup-wait can't desync anything.
2. **`onLoaded`** (lcd task) drops the bytes into an in-RAM cache
   (`unordered_map<abs-path, Blob>`) and calls `lcdLauncherIconLoaded(basename)`.
3. A tiny **`lv_fs` driver on letter `'D'`** serves LVGL's image decoder straight
   from that cache, so `lv_image_set_src("D:/fixed/lcd/icons/36x36/rns.bin")`
   works with zero flash on the lcd task.

The resolution is fixed to `LAUNCHER_ICON_RES` (`36x36`, the tile size), so
`lcdIconResRefresh()` is a permanent no-op (returning false) and `s.lcd.icon_res`
is inert — a device with an old persisted value still renders. `lcdLauncherReload`
and the `s.lcd.icon_res` subscription remain wired but never fire usefully.

## 8. Inactivity, standby, boot reveal

The inactivity timer (`lcd_lvgl.cpp`, armed by the `s.lcd.inactivity_timeout`
subscription) sets the ephemeral `sys.standby` key on expiry; the **board** owns
what standby means and calls `lcdScreenSleep()`/`lcdScreenWake()` off that key.
`lcdActivity()` re-arms the timer on every input edge. The backlight is held dark
from boot and faded up only once launcher icon loads go quiet (`lcdBootSettleKick`,
debounced with a hard cap), so the UI never flashes on half-built.

## 9. The stylesheet

`stylesheet.h` is theme + geometry as data: one `LcdStyle` per (name, screen
size), selected at `lcdStyleBegin(w, h)` by the real panel size and calibrated
(percent → px). The shell reads geometry/colour/font from `lcdStyle()` instead of
`#define`d magic numbers, so a second board is a new sheet, not code. Only the
320×240 dark sheet ships (`stylesheet_320x240.cpp`): status bar 24 px on dark
navy, a 4×3 tile grid of 72×64 tiles with a 36 px icon, the `36x36` icon bucket,
and the recents/nav/gesture thresholds. `LcdStyle::core.maxResidentApps` (4) is
declared as an eviction cap but is **not currently enforced** — eviction is only
user-driven via a recents swipe-up.

## 10. Pitfalls

- **Never do flash I/O on the lcd task** — it breaks the `itsPoll` notification
  accounting. Read elsewhere (the loader) and hand bytes over with `lcdRun`.
- **Hold the shared-bus lock across the whole panel DMA**, not just the queue —
  see §3; dropping it early panics a co-resident SPI driver.
- **Set the foreground before `onCreate`** so an app's chrome flips bind to
  itself, not the previous foreground (mirrors the legacy `s_current`-first order).
- **`Config::navBar` and `Config::fullscreen` are not consumed** — only
  `statusBar`, `name`, `iconBasename`, and `launcherPage` are read at install.
  Fullscreen is a runtime flag set via `setFullscreen()`; there is no on-screen
  nav bar renderer yet (navigation is gesture + ESC + board button).
- **`setStatusIcon` / `setRecentsSubtitle` are stubs/unrendered** — don't present
  them as working chrome.
- **PSRAM-stack tasks must not `printf`** — use `info()`/`warn()`/etc.
- **A stylesheet sheet must be `extern const`** — a namespace-scope `const` has
  internal linkage by default, so `stylesheet.cpp`'s registry can't see a sheet
  defined in another TU (`stylesheet_320x240.cpp`) unless that definition is
  declared `extern const`. A new board's sheet has to do the same or it won't
  link into the registry.
- **The status bar lives on `lv_layer_top()`**, always frontmost; program layers
  and the launcher are screen children below `statusBar.h`. The top layer is kept
  click-through except the small home-bar patch.
