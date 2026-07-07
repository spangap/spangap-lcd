# Settings — internals

Maintainer reference for `shell/../lcd_settings.cpp` — the built-in Settings
app, its menu registry, the page-stack nav, the `lcdSetting*` helpers, and the
two-way storage binding. The [operator guide](settings.md) is the author view.
Everything runs on the lcd task.

## 1. What settings adds

- **`lcdRegisterSettings(path, label, fn, placement)`** — builds an in-RAM menu
  tree of `Node`s by splitting the slash-path; intermediate segments are
  auto-titled submenus, the leaf gets `fn` + the explicit label + placement. A
  plain tree, so registration works before `lcdInit()` and from any task.
- **The `lcdSetting*` helpers** — storage-bound row builders.
- **`SettingsApp`** — a thin `LcdApp` host (gear tile, `Config::name = "Settings"`,
  `iconBasename = "gear"`, `launcherPage = 0`) installed by `lcdSettingsInit()`
  from `shellInit`. Its `onCreate` calls `settingsOpen(root)`; `onClose` clears
  the page-stack pointers. The registry, the builders, two-way binding, scroll
  pills, and every straddle's pane hook are unchanged by the app wrapper.
- **The built-in Display/UI Zoom pane** — `lcdSettingsInit()` also registers
  `"display/zoom"`: a −/+ stepper (25% steps, clamped 50–200) over
  `s.lcd.scale`. It only writes the key; the reflow is the `lcd.cpp`
  subscription → `shellApplyZoom()` (shell-internals §10), so a browser/CLI
  write behaves identically.

## 2. The page-stack nav

Each menu level and each item pane is its own opaque, full-size **page** stacked
in a host container below a shared header. Descending pushes a new page **on
top** — the parent stays alive, untouched, beneath it; Back deletes the top page,
revealing the parent exactly as it was (scroll position included). The header
(back chevron + breadcrumb title, e.g. `Settings/Net/Wifi`) lives **outside** the
pages, so Back never deletes the widget whose event it's handling, and descending
never deletes the row being clicked.

- `pushMenu(node)` renders a page of rows (one per child), sorted by `placement`
  bucket then case-insensitive label (registration order is dependency order,
  meaningless to the user). A submenu row gets a chevron; an item row carries the
  `Node*` and dispatches on click.
- `pushItem(node)` makes a page and calls `node->fn(page)` to build the pane.
- `popPage()` deletes the top page; at the root it exits via `lcdGoHomeInternal()`.
- **Scroll-overflow pills** — small `↑`/`↓` chips at the host's right edge,
  shown from the top page's scroll bounds, re-floated above each freshly pushed
  page. All pages scroll (a menu can outgrow the viewport too); LVGL's
  scroll-vs-tap threshold keeps row clicks working.

The breadcrumb in `s_titleLbl` uses `lcdFont(LcdFace::UI_BOLD, 16)`; menu rows
use `lcdFont(LcdFace::UI, 16 × lcdUiScale())`.

## 3. Two-way storage binding

Every storage-bound control registers a `Bind { key, widget, kind, secret }` in
the file-static `s_binds`. The control's `LV_EVENT_VALUE_CHANGED` writes the key
(`storageSet`); separately the key is **subscribed** (`storageSubscribeChanges` →
`bindDispatch`) so an external write flows back into the widget via `bindApply`,
switched on `BindKind` (`BK_SWITCH`/`BK_SLIDER`/`BK_DROPDOWN`/`BK_TEXTLBL`/
`BK_TEXTAREA`/`BK_VALUE`). Storage callbacks dispatch on the lcd task, so
`bindApply` touches LVGL directly; no locks.

`bindAttach` subscribes the key only on its first bind; `bindDelete`
(`LV_EVENT_DELETE`, fired as page nav destroys widgets) removes the bind and
unsubscribes the key once its last bind goes — so nothing leaks across
navigation. The `BK_TEXTAREA` case won't clobber a field that is currently
focused (being edited).

**Key-by-pointer.** Binds store the key as a raw `const char*` (`Bind::key` is a
`std::string` copy, but the helpers pass the caller's pointer to the event
`user_data`). Pages are created and destroyed on navigation, so keys must be
string literals / static — this is the lifetime warning in the operator guide,
and the reason the registry deliberately doesn't `strdup`.

## 4. Helper specifics

Rows share the `makeRow`/`addRowLabel` scaffolding: a flex row with a 1/3-width
right-aligned label column and the control(s) in the remaining 2/3,
left-aligned so a control sits next to its label rather than pushed to the far
edge (`fillRowControl` stretches a control across the 2/3 where that reads
better — dropdown / value / slider group).

- **Switch** — a compact `lv_switch` (36×18) with a high-contrast off state.
- **Slider** — a `lv_slider` plus a live numeric readout label; both bind the key
  (`BK_SLIDER` + `BK_VALUE`) so an external write refreshes the number too.
- **Text** — two paths on `lcdHasKeyboard()`: inline `lv_textarea` (joined to the
  focus group, committing on `LV_EVENT_READY`/`DEFOCUSED`) vs a full-screen
  `lv_keyboard` overlay over a value label. The overlay's `TextRef` is
  `gp_alloc`'d and freed on the row's `LV_EVENT_DELETE`.
- **Dropdown** — CSV → newline options; `dropdownSelect` matches the stored value
  by option text.
- **Value** — a label bound `BK_VALUE`, event-driven (the em-dash `—` for empty).
  Long-value rendering is the marquee/wrap split below.
- **Button** — `onClick` is an `lcd_fn_t` invoked with the row as `arg`.

## 5. The marquee tunable (`CONFIG_LCD_SETTINGS_MARQUEE`)

`lcdSettingValue` has two layouts behind the Kconfig:

- **On** (default) — `valueLabelMarquee` makes the value single-line, ellipsized
  (`LV_LABEL_LONG_DOT`), `flex_grow`-bounded, and keypad-focusable; a
  FOCUSED/DEFOCUSED handler flips it to `LV_LABEL_LONG_SCROLL_CIRCULAR` only while
  focused. Only the focused row marquees (a panel of hashes all scrolling at once
  would be noise). Needs a focus ring to drive it.
- **Off** — a value longer than 18 chars is stacked: the label over a wrapped
  value (`LV_LABEL_LONG_WRAP`), for touch-only boards.

## 6. Pitfalls

- **Keys are stored by pointer** — see §3. A `std::string::c_str()` from a
  temporary dangles after the pane is rebuilt.
- **Don't rebuild a page in place during a live click** — the page-stack model
  exists because the old rebuild-in-place scheme cleaned content out from under
  the click event being handled. Push/pop pages; never clear the live page.
- **`s_pages` holds raw page pointers** — `SettingsApp::onClose` clears it so an
  evicted-then-reopened Settings doesn't dereference deleted pages.
- A few `dbg(...)` "TEMP diag" traces remain in `pushMenu`/`onRowClick`; they are
  verbose-level and harmless, but are diagnostics, not features.
