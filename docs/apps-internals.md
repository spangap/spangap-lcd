# Apps — internals

Maintainer reference for the `LcdApp` model (`shell/lcd_app.cpp`) and the
built-in apps (`apps/`). The [operator guide](apps.md) is the author view. The
shell state machine that drives these calls is in
[shell-internals.md](shell-internals.md). Everything runs on the lcd task.

## 1. What the app model adds

- **`LcdApp`** ([lcd_app.h](../esp-idf/include/lcd_app.h)) — the base class. It is
  a boot-registered `Service` (service.h): the UI lifecycle virtuals
  (`onCreate`/`onShow`/`onHide`/`onBack`/`onClose`) plus a `Config`, the per-app
  service methods, the resource ledger, and shell-private accessors (the
  `_`-prefixed ones). Its **boot** lifecycle is fixed: `onInit()` is `final` — it
  hops onto the lcd task and calls `lcdInstall(this)`, so a straddle installs an
  app purely by listing the class in `services:` (no install hook). App-level boot
  wiring (CLI verbs, worker spawn) goes in the `appInit()` override, run on the
  boot task straight after the tile is installed.
- **`lcdInstall(LcdApp*)`** (`lcd_app.cpp`) — the install registry: pushes the app
  onto the file-static `s_apps`, assigns an id, and calls `shellLauncherAddTile`.
  Called from `LcdApp::onInit` for registered apps, and directly by `shellInit`
  for the built-ins (constructed on the lcd task, never registered as services).
- **`NavIntent`** — the navigation request enum (`BACK`/`HOME`/`RECENTS`),
  decoupling producers (gesture, ESC, board button, nav bar) from the single
  consumer `shellNavigate`.
- **The built-in `apps/`** — `log_app.cpp` and `cli_app.cpp` (plus the
  `SettingsApp` host in `lcd_settings.cpp`).

`s_apps` is the master list; the **running set** (recents) is the subset whose
`root()` is non-null. `shellApps()` exposes it; `shellFindApp(name)` is the
`lcdShowProgram` lookup.

## 2. Lifecycle mapping & ownership

The lifecycle maps onto the shell points (see shell-internals §4):

| Method | Called when |
|---|---|
| `onCreate(root)` | first open (lazy build into the freshly made program layer) |
| `onShow()` | every open after build (foreground raise) |
| `onHide()` | sent to background (another app opened, or Home slide-up completes) |
| `onBack()` | a BACK intent reached the foreground app |
| `onClose()` | eviction (recents swipe-up) — before the ledger + root are freed |

**The shell owns the instance; it never deletes it** — only the root layer is
evictable, and a later open rebuilds it. So `LcdApp` subclasses must be heap- or
static-allocated and must not assume their destructor runs at eviction (it
doesn't; `onClose` is the teardown hook). State that must survive eviction lives
outside the object (storage, file statics).

## 3. The resource ledger

Timers and animations have no LVGL owner, so a closed app would leak them.
`timer()` records every `lv_timer_t*`; `anim()` returns a tracked zeroed scratch
`lv_anim_t`, and `startAnim()` records `{var, exec_cb}` **before** LVGL copies the
descriptor in. `_reclaimLedger()` (called by `shellEvictApp` after `onClose`)
`lv_timer_delete`s and `lv_anim_delete`s them all and frees the recents thumbnail.
Objects are deliberately *not* ledgered — `lv_obj_delete(root)` frees the subtree.

A one-shot self-deleting timer (repeat count 1) needs no ledger; the CLI app uses
one for deferred focus (§5).

The ledger is the deliberate alternative to how the Brookesia donor frees a
closed app's resources: walking LVGL's *private* `_lv_anim_ll` linked list (its
`core/esp_brookesia_core_app.cpp`) — the donor's one coupling to LVGL internals,
and brittle across LVGL 9.x. The ledger gives the same guarantee — a closed app
can't leak — with zero LVGL-internals coupling.

## 4. Service method delegation

The service methods are thin delegators to the manager so an app never touches
"the currently shown layer":

- `inputGroup()` → `lcdInputGroup()`.
- `goHome()` → `shellNavigate(HOME)`.
- `setFullscreen(on)` / `setScrollwheelArrows(on)` set `m_fullscreen`/`m_arrows`
  and call `shellAppChanged(this)`, which re-applies chrome **only if this app is
  the foreground** — so a background app can flip a flag harmlessly.
- `setScrollHandler(fn)` sets `m_scrollFn`, consulted by `lcdScroll`.

The UI-state flags (`_fullscreen`/`_arrows`/`_scrollFn`) ride on the app across
hide/show (the manager reads them in `applyChrome`), as the legacy launcher's
per-entry flags rode the layer.

## 5. Built-in apps (apps/)

Both Log and CLI are direct ports of the legacy programs and share a shape: the
ITS connection lives for the **life of the layer**, not its visibility, so an app
hidden behind another keeps receiving and re-opening shows current state.
`onCreate` builds the view and opens the connection; `onClose` closes it. ITS
recv/disconnect callbacks carry no user pointer, so each app's connection state
lives in a file-static (there is exactly one instance of each).

- **`log_app.cpp`** — a virtualized text view ([terminal.md](terminal.md)) in
  `lcdFont(LcdFace::MONO, 8)` (which resolves to Spleen 5×8 through the
  small-mono bitmap band), an ITS client of the log task's `log:1` DC port with connect
  payload `{"ansi":0}` (plain text, no ANSI escapes LVGL can't render).
  Per-line severity colour via `lcdTextViewSetLineColor` (the scrollback stays
  plain text, so the column math never sees colour). Scrollback is capped to
  `s.log.file.paste` kB (min 4). `onShow` scrolls to the tail.
- **`cli_app.cpp`** — a real VT100/xterm terminal (`lcd_term`/libvterm) on the
  `cli:1` DC port, so a shell / top / vim lay out correctly. It calls
  `setScrollwheelArrows(true)` so the trackball drives arrow keys while it's up,
  and sends the terminal grid size as the connect payload. **Deferred focus:** the
  launcher tile that opened the app grabs the focus group on click-release (after
  the app's handlers), so an immediate focus is stolen back — a one-shot 40 ms
  timer re-focuses the terminal once that settles.

## 6. Pitfalls

- **The instance is never deleted** — don't put eviction teardown in the
  destructor; use `onClose`. Don't stack-allocate an `LcdApp`.
- **`onCreate` runs at most once per resident lifetime** — after eviction it runs
  again on the next open, so it must be a clean rebuild (don't depend on member
  state set in a previous incarnation; the object persists but the LVGL tree
  doesn't).
- **ITS callbacks carry no userdata** — the built-in apps rely on being
  singletons (file-static connection state). A multi-instance app would need its
  own correlation.
- **`Config::navBar`/`fullscreen`, `setStatusIcon`, `setRecentsSubtitle` are not
  wired** — see shell-internals §11.
