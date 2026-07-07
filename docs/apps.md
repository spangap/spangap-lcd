# Apps — the LcdApp model

An on-device app is one launcher program with a real lifecycle: a subclass of
`LcdApp` ([lcd_app.h](../esp-idf/include/lcd_app.h)) that owns its own root layer
and overrides the lifecycle methods it cares about. `LcdApp` is a boot-registered
`Service`, so you install an app by listing it in your straddle's `services:`
block — the generated boot code constructs it and installs its launcher tile, and
the shell owns it from there. Maintainer detail is in
[apps-internals.md](apps-internals.md).

## Installing an app

Define the class in a public header (global, no namespace — the generated
trampoline needs external linkage) with a **default constructor** that does member
init only. It runs at the very top of `app_main`, before any bring-up, so the ctor
must not touch storage/fs/log/cli/ITS:

```cpp
// myapp.h
#include "lcd_app.h"

class MyApp : public LcdApp {
public:
    MyApp() : LcdApp({ .name = "My App", .iconBasename = "myicon" }) {}

    void onCreate(lv_obj_t* root) override {   // build UI once, on the lcd task
        lv_obj_t* lbl = lv_label_create(root);
        lv_label_set_text(lbl, "hello");
        lv_obj_center(lbl);
    }

protected:
    void appInit() override {                  // optional boot-task wiring
        cliRegisterCmd("myapp", myappCli);     // CLI verbs, mutexes, worker spawn
    }
};
```

Then register it in `straddle.yaml` — one line, `when:`-gated on `spangap-lcd` so
the app compiles only when the shell is in the build:

```yaml
services:
  - { class: MyApp, header: myapp.h, when: spangap/spangap-lcd }
```

That is all — no hand-written install hook. The generated boot code constructs
`MyApp`, and `LcdApp::onInit` (which is `final`) hops onto the lcd task and calls
`lcdInstall()` for you, so a registered app always gets its tile. Boot wiring that
isn't the UI — CLI verbs, mutex creation, worker-task spawn, storage subscriptions
— goes in `appInit()`, which runs on the **boot** task right after the tile is
installed (the safe place for `cliRegister`/`spawnTask`). The shell owns the
instance and never deletes it (only its root layer is evictable), so it is never
stack-allocated; the trampoline `new`s it once and it lives forever.

`lcdInstall()` stays available for the shell's own built-ins (Settings, Log, CLI),
which are constructed on the lcd task and never registered as services; an
ordinary straddle app never calls it directly.

### Config

`LcdApp::Config` is passed to the base constructor:

| Field | Default | Meaning |
|---|---|---|
| `name` | `""` | Launcher label **and** lookup key for `lcdShowProgram()`. A string literal. |
| `iconBasename` | `""` | Icon SVG under `/fixed/icons/<name>.svg`, by basename (no path/ext); rasterized on the device at the tile size. |
| `statusBar` | `true` | Keep the status bar visible while this app is shown. |
| `launcherPage` | `0` | Which launcher page the tile lands on. |

(`navBar` and `fullscreen` also exist in `Config` but are not yet consumed —
request fullscreen at runtime with `setFullscreen()` instead.)

## The lifecycle

Every method runs on the lcd task. The shell calls them at these points:

- **`onCreate(root)`** (required) — build the UI **once**, into `root`. Called
  lazily the first time the app is opened (or again after eviction). Build your
  whole tree here; the layer persists across hide/show.
- **`onShow()`** — the app came to the foreground (every open after the first).
- **`onHide()`** — the app went to the background (another app opened, or Home).
- **`onBack()`** — a Back navigation arrived; return `true` if handled, `false`
  to let it fall through to Home (the default).
- **`onClose()`** — the app was evicted (terminated from recents). Tear down
  external resources here; the resource ledger and the root tree are freed for
  you afterward, and the next open rebuilds from `onCreate`.

The layer persists after `onCreate` returns: opening another app hides yours,
re-opening reveals it exactly as left — your widgets are still there and
`onCreate` does **not** run again.

## Services

Valid from `onCreate` onward (the root exists). They operate on **this** app, not
"the currently shown layer", so there's no ordering trap:

- `root()` — the app's root layer (also the `onCreate` argument).
- `inputGroup()` — the shared keypad focus group. Add your focusable widgets
  (e.g. a `lv_textarea`) with `lv_group_add_obj(inputGroup(), w)` so the hardware
  keyboard / button can reach them. The shell saves and restores your app's
  focus automatically across hide/show.
- `goHome()` — return to the launcher (e.g. wire it to a Back button).
- `setFullscreen(on)` — hide the status bar and grow the layer to full height for
  an immersive screen; toggle it as your own view changes. The shell remembers
  which app asked and restores the bar when it goes Home.
- `setScrollwheelArrows(on)` — while this app is shown, the trackball emits arrow
  keys into the focus group instead of moving the pointer (for a terminal / vim).
- `setScrollHandler(fn)` — for content that isn't an LVGL scroll container (e.g. a
  map canvas you reposition yourself): edge-pan deltas (`lcdScroll`) are handed to
  your `fn(dx, dy)` instead of scrolling a widget. Signs follow the finger-drag
  convention. Apps built from ordinary LVGL scroll containers need none of this —
  edge-pan finds and scrolls them automatically.

`setStatusIcon()` and `setRecentsSubtitle()` exist on the base class but are not
yet rendered.

## Fonts in an app

Labels inherit the platform UI font from the theme — most app text needs no
font call at all. Where you do want a specific face or size, resolve it through
`lcdFont()` (`lcd.h`) rather than naming a compiled font, and multiply your
base size by `lcdUiScale()` so it follows the platform zoom:

```cpp
lv_obj_set_style_text_font(title,
    lcdFont(LcdFace::UI_BOLD, (int)(16 * lcdUiScale() + 0.5f)), 0);
```

The `LV_SYMBOL_*` glyphs work in any `lcdFont()` face (the symbol face is
chained as a fallback). A zoom change does not restyle an open app's explicit
fonts — re-resolve them when you rebuild (the shell reflows its own chrome; see
[shell.md](shell.md#ui-zoom)). The nomad straddle's `lcd/` slice is the worked
example: its Micron page renderer keeps a user-steppable ladder of
`lcdFont(LcdFace::MONO, px)` sizes (10/12/14/16/20 px, persisted as an index in
`s.nomad.page_font`, deliberately *not* zoom-scaled — the ladder is the density
control) while its chrome font is `lcdFont(LcdFace::UI, 14 × lcdUiScale())`,
re-resolved at the top of every rebuild.

## Resource ledger

Timers and animations live in LVGL globals with no owner, so a closed app would
leak them. Create them through the app and the shell frees them on eviction:

```cpp
lv_timer_t* t = timer(myCb, 1000);   // tracked; deleted on onClose
lv_anim_t*  a = anim();              // a zeroed, tracked scratch anim
lv_anim_set_var(a, root()); /* …configure… */ startAnim(a);
```

Objects need no ledger — deleting `root()` frees the whole tree. Use the ledger
only for timers and animations.

## Built-in apps

The shell installs three apps out of the box: **Settings** (the gear — see
[settings.md](settings.md)), **Log**, and **CLI**. Log and CLI are ITS clients
of the device's log and CLI tasks, rendered with the on-device terminal widgets —
see [terminal.md](terminal.md). Other straddles install their own apps the same
way.

## Opening an app programmatically

`lcdShowProgram(name)` brings an installed app to the front by its `Config::name`
(building it on first use), from on the lcd task. `lcdGoHome()` returns to the
launcher from any task.
