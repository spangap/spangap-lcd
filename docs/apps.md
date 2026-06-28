# Apps — the LcdApp model

An on-device app is one launcher program with a real lifecycle: a subclass of
`LcdApp` ([lcd_app.h](../esp-idf/include/lcd_app.h)) that owns its own root layer
and overrides the lifecycle methods it cares about. You hand an instance to
`lcdInstall()`; the shell owns it from there. Maintainer detail is in
[apps-internals.md](apps-internals.md).

## Installing an app

```cpp
#include "lcd_app.h"

class MyApp : public LcdApp {
public:
    MyApp() : LcdApp({ .name = "My App", .iconBasename = "myicon" }) {}

    void onCreate(lv_obj_t* root) override {
        lv_obj_t* lbl = lv_label_create(root);
        lv_label_set_text(lbl, "hello");
        lv_obj_center(lbl);
    }
};

// from your straddle's init (on the lcd task), guarded on CONFIG_SPANGAP_LCD:
lcdInstall(new MyApp());
```

`lcdInstall(app)` adds the app's launcher tile and records it; it must run on the
lcd task (call it from an init the build already routes there, or wrap in
`lcdRun()`/`ON_LCD`). The instance must outlive the process — the shell never
deletes it (only its root layer is evictable), so allocate it with `new` or as a
static, never on the stack.

### Config

`LcdApp::Config` is passed to the base constructor:

| Field | Default | Meaning |
|---|---|---|
| `name` | `""` | Launcher label **and** lookup key for `lcdShowProgram()`. A string literal. |
| `iconBasename` | `""` | Icon under `/fixed/lcd/icons/36x36/<name>.bin`, by basename (no path/size/ext). |
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
