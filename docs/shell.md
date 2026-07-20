# Shell — launcher, status bar, navigation, recents

The shell is the phone-style chrome around the on-device apps: an opaque status
bar at the top, a paged launcher of app tiles, a home-bar gesture strip at the
bottom, and a recents switcher. It runs a single foreground/back/home state
machine — one app in the foreground at a time, the launcher as the root.

This is the only shell; it comes up unconditionally when `spangap-lcd` is in the
build. Apps plug into it through the [`LcdApp`](apps.md) model; this document is
the operator/author view of the chrome itself. Maintainer detail is in
[shell-internals.md](shell-internals.md).

## The screens

The shell shows exactly one of three screens at a time:

- **Launcher** — the home screen: a horizontal pager of pages, each a grid of
  app tiles, with a page-indicator dot row. Tapping a tile (or focusing it with
  the keypad/trackball and clicking) opens that app. The grid derives from the
  viewport: as many columns as minimum-size tiles fit the width, rows from the
  tile height, so a different panel or zoom level reflows it with no
  configuration. With a handful of apps one page suffices; a tile lands on the
  page its app asked for (`Config::launcherPage`) — a *request*, not a hard
  slot: a full page spills the tile to the next, and a new page is created on
  demand.
- **App** — one app's full-screen layer, below the status bar (or reclaiming it
  when the app is fullscreen). Opening another app hides this one; it keeps
  running in the background and re-opening resumes it exactly as left.
- **Recents** — a modal switcher (below) over the running set.

## The status bar

An opaque bar across the top, always frontmost, rendered from storage keys and
purely event-driven (no polling):

- **Clock** (left) — `strftime(s.lcd.date_format)`; hidden until `sys.time.valid`.
  It re-renders only when the shown value changes — once a minute for a
  minute-resolution format, every second only if the format includes seconds.
- **Wi-Fi glyph** (right) — `LV_SYMBOL_WIFI` whose opacity tracks association
  state and RSSI (`wifi.sta.state` / `wifi.sta.rssi`).
- **Upstream dot** (right) — a small dot, green when the device has working
  internet (`wifi.sta.up`), grey otherwise. Distinct from the Wi-Fi glyph, which
  only reports association.
- **Battery** (right) — a battery glyph from `battery.percent`, red below ~12%;
  hidden entirely on a board that publishes no `battery.percent`.
- **Straddle indicators** (right, left of the battery) — any straddle may add its
  own indicator with `lcdStatusbarAddIndicator()` (public, `lcd.h`), which returns
  an empty content-width slot the caller fills with whatever LVGL children it
  likes and updates off its own storage subscription. The shell only positions
  the slot; it never interprets the content, so it stays agnostic to what any
  straddle chooses to display.

The clock format key is owned here (`s.lcd.date_format`); the rest are read from
keys other straddles publish (see the [README](../README.md#storage-variables)).

A fullscreen app reclaims the bar's rows for an immersive screen — see
[apps.md](apps.md). The per-app status-bar icon API (`LcdApp::setStatusIcon`) is
declared but not yet rendered; don't rely on it.

## Navigation

Three producers all funnel into one navigation action (Back / Home / Recents):

- **Home-bar gesture** — a swipe up from the bottom strip. How far it travels at
  release decides the outcome: a long swipe (past ~60% of the screen) goes
  **Home** (the launcher fades in as the app slides up off the top); a medium
  swipe (~20–60%) opens **Recents**; a short swipe springs the app back.
- **ESC key** — the keyboard's Escape means **Back**, but only where it would
  otherwise do nothing useful: it is passed through to a terminal/arrow-mode app
  or a focused text field, so it never steals an editing key.
- **Board Home button** — the board's `click_read` long-press calls `lcdGoHome()`.

**Back vs Home.** Back asks the foreground app first (`onBack()`); if the app
doesn't handle it, Back falls through to Home. Home always returns to the
launcher. At the launcher both are no-ops. `lcdAtLauncher()` lets a board tell
the two apart (e.g. to go straight to standby on a second Home press).

## The recents switcher

Recents is a modal overlay listing the **running set** — the apps whose screen
currently exists (built and not yet evicted). Each app is a horizontal card
showing a half-scale live snapshot of its screen (captured the moment it left
the foreground) with its name below; an app with no snapshot yet falls back to
its launcher icon. A memory readout (internal / external heap free/total) sits at
the top as an on-device diagnostic.

- **Tap a card** to switch to that app.
- **Swipe a card up** to terminate it — the app's `onClose()` runs, its screen
  and resources are freed, and it drops out of the running set (next open
  rebuilds it from scratch).

Recents sits over the foreground app: Back or Home dismisses it back to that app
(or the launcher if none) without disturbing it. Closing the last running app
returns to the launcher.

Terminating an app from recents is the only thing that evicts it today; there is
no automatic memory-pressure eviction.

## UI zoom

The whole interface scales at runtime: **Settings → Display → UI Zoom** is a
−/+ stepper over `s.lcd.scale` (percent, 25% steps, clamped 50–200; default
100). Writing the key from anywhere — the stepper, the browser, the CLI — has
the same effect. A change *reflows* rather than magnifies: fonts are
re-rasterized from their vector faces at the new size, icons are re-rendered
from their SVG sources, and the launcher grid recomputes its columns and tile
size — so everything stays crisp at every factor, with bigger tiles and fewer
per page as you zoom in. An app that is already open keeps its old-size fonts
until it is next rebuilt; apps that resolve their fonts through
`lcdFont(face, basePx × lcdUiScale())` pick the new scale up on their next
build (see [apps.md](apps.md)).

## Programmatic control

From on the lcd task (a registered callback, an `lcdRun()` block, an app
method):

- `lcdGoHome()` — return to the launcher (safe to call from any task; it hops on).
- `lcdShowProgram(name)` — bring a registered app to the front by its
  `Config::name`, building it on first use; a no-op if no such app exists.
- `lcdAtLauncher()` — true when the launcher is the current view.

Apps generally use the `LcdApp` service methods (`goHome()`, `setFullscreen()`,
…) instead of these free functions — see [apps.md](apps.md).
