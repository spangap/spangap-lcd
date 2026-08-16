# Settings — the on-device settings menu

The shell installs a built-in **Settings** app (the gear tile) hosting **one
tree** of storage-bound nodes — the on-device counterpart of the browser's
settings tree, built from the same `settings:` blocks. Maintainer detail is in
[settings-internals.md](settings-internals.md).

Almost nothing should reach for this API directly: a straddle describes its
settings in `straddle.yaml` and the build lowers them here, which is why the
`settings:` block is where to look first. What follows is the surface that
lowering targets, and the one a genuinely custom pane still uses.

## One tree, not a set of panes

Every node holds **rows** and **children**: entering it renders the rows first,
then a navigation row per child. There is no leaf/container distinction, and no
node is owned — a contribution names a path of ids, every intermediate node on
the way is conjured, and several contributors at the same path concatenate their
row blocks.

```cpp
#include "lcd.h"

static const lcd_seg_t kAt[] = {
    { .id = "network", .label = "Internet", .shortName = "Internet", .order = 2, .has_order = true },
    { .id = "wifi",    .label = "WiFi",     .shortName = "WiFi",     .order = 1, .has_order = true },
};

lcdSettingsContribute(kAt, 2, ON_LCD {
    lv_obj_t* pane = (lv_obj_t*)arg;             // empty scrollable flex column
    lcdSettingSection (pane, "WiFi");
    lcdSettingSwitch  (pane, "Enable", "s.net.wifi.enable");
    lcdSettingValue   (pane, "Status", "wifi.sta.state");
    lcdSettingValue   (pane, "IP",     "wifi.sta.ip");
    lcdSettingSection (pane, "Access Point");
    lcdSettingText    (pane, "Name",     "s.net.wifi.ap.ssid");
    lcdSettingText    (pane, "Password", "s.net.wifi.ap.pass", /*secret*/true);
});
```

- **`id`** is the stable merge key — two contributors sharing a node must spell
  it identically. It is also the node's path id on both surfaces.
- **`label`** is the long name, shown in the parent's navigation row;
  **`shortName`** is the header text and defaults to the label. The **first**
  contributor to supply each wins; a later differing name is ignored (the build
  warns about the conflict).
- **`order`** places a node among its siblings. One rule everywhere: nodes
  carrying an order first, ascending; the rest after them in contribution order
  (which is init order — the platform's nodes land before a consumer's).
- **`fn`** is called on the lcd task with the node's content pane (an empty
  scrollable flex column); it may be null for a contribution that only names
  nodes.

Call at init, from any task — it populates an in-RAM registry, so it works even
before `lcdInit()`.

## Two conventions the rows assume

**The firmware publishes finished strings.** A value row, a subtitle, a status
pill: whatever the key holds is shown verbatim. Nothing on this side formats,
composes or compares — so a derived display value (signal wording, a composed
counter, a formatted percentage) is written to an ephemeral key ready to render.
A gate key is published truthy or empty, because `lcdSettingWhenKey` tests
truthiness and never equality.

**The firmware validates in sentinel handlers.** A form submits its fields as
JSON to a command key; the owning task validates and answers on the sentinel
family's shared keys: a rejection is a human-readable reason on `<cmd>.error`
(the form shows it and stays open), an accepted mutation bumps `<cmd>.done`
(the form closes — an edit that changes nothing still acks). There is no
validation on this side and no per-keystroke checking anywhere.

## The pane helpers

Each builds a uniform row bound to a storage key and runs on the lcd task inside
a settings `fn`. They mirror the browser's `Setting*` components.

| Helper | Control | Bound to |
|---|---|---|
| `lcdSettingSection(parent, title)` | bold section divider | — |
| `lcdSettingCaption(parent, text)` | greyed, wrapped help text | — |
| `lcdSettingSwitch(parent, label, key)` | toggle | int key (0/1) |
| `lcdSettingSlider(parent, label, key, min, max)` | slider + live numeric readout, clamped | int key |
| `lcdSettingText(parent, label, key, secret=false)` | text field (inline edit or on-screen keyboard) | string key |
| `lcdSettingDropdown(parent, label, key, optionsCsv)` | dropdown | string key |
| `lcdSettingValue(parent, label, key)` | read-only, live | string key |
| `lcdSettingButton(parent, label, onClick)` | action button | — (`onClick` on lcd task) |
| `lcdSettingWhenKey(row, key)` | wraps any row above; shows it while the key is truthy | string key |
| `lcdSettingAction(parent, label, act)` | button running a descriptor action | — |
| `lcdSettingCollection(parent, desc)` | an array, as an editable list | the descriptor's array key |

**Writes apply immediately** — there is no "save". A switch flips its key the
instant it's toggled.

**Two-way bound.** Every control is also *subscribed* to its key, so an external
write (browser, CLI, another task) flows back into the control: flip a switch in
the browser and the on-device switch follows, and vice versa. `lcdSettingValue`
is purely event-driven off this subscription (no polling).

**Text entry** adapts to the hardware. When a consumer has reported a keyboard
(`lcdSetHasKeyboard(true)`), `lcdSettingText` edits in place — the value is an
inline one-line textarea; focus it, type, Enter or moving away commits.
Otherwise it opens a full-screen on-screen keyboard. `secret` masks the value.

> **Key lifetime.** Storage keys passed to the helpers are stored **by pointer**,
> not copied — panes are rebuilt on every navigation. Pass string literals /
> static storage, never a temporary `std::string`'s `.c_str()`.

For a custom focusable control outside the helpers, add it to `lcdInputGroup()`
(`lv_group_add_obj`) so the keyboard/keypad can reach it.

## Long values

A long read-only value (an identity or destination hash, a path) doesn't fit a
single-line row. With `CONFIG_LCD_SETTINGS_MARQUEE=y` (the default), such a value
is ellipsized and made keypad-focusable: landing on it with the trackball/keypad
horizontally scrolls it (only the focused row), then re-ellipsizes when you move
off. On a touch-only board with no focus ring, turn the option off and a long
value instead wraps onto a second line under its label (tap-to-read).

## Actions, dialogs, forms and collections

Anything richer than a labelled row is described by a **descriptor struct**
(`lcd_settings_desc.h`) and run by generic code, rather than written per pane.
The build emits these from a straddle's `settings:` block; a hand-written pane
may equally declare one as a `static const` and call the same runtime.

An **action** (`lcd_action_t`) is one of three things, and `lcdSettingAction()`
puts a button in front of it:

- **set** — write a key. `edge` writes `0` first, forcing a change past the
  storage actor's dedup, which a command flag left set by an incomplete attempt
  needs or it will swallow every later press. `reboots` shows a modal notice
  afterwards, because the device is going away.
- **dialog** — a confirmation or choice, with no input fields ever. Every button
  closes it; a button with no action is a cancel. Buttons nest actions, so a
  choice tree is dialogs of buttons of writes.
- **form** — the one dialog with inputs, because it fronts a sentinel. Fields
  are ordinary rows carrying `field` instead of `key`; values live in a local
  buffer and reach the device as one JSON object on submit. On a board with a
  physical keyboard the fields are inline textareas, otherwise tapping one opens
  the on-screen keyboard — one form serves both paths, so nothing branches on
  `lcdHasKeyboard()` any more.

A **collection** (`lcd_collection_t`) renders the array at its `key` as titled
rows with an optional subtitle and status pill, per-item action buttons, an item
editor, removal and — with `orderable` — up/down reordering. It **never writes
the array**: it writes `<cmd>.add`, `<cmd>.remove`, `<cmd>.set` and
`<cmd>.order`, and the owning task is the array's only writer. A reorder sends
the complete id order comma-joined, which the firmware applies as a *preference
permutation* (recognized ids into that relative order, unknown ids ignored,
unmentioned ids left in place) — idempotent, and harmless against a concurrent
edit.

A `candidates` clause adds scan-and-adopt: an ephemeral array the task
publishes, rendered like the item rows, where picking one opens the first add
form prefilled. Leaving the pane clears the refresh target key, so nothing has
to carry a "stop scanning on leave" timer.

Everything here is event-driven — callbacks and storage subscriptions, no waits
and no polling. `storageSet` is async (it queues to the owning actor), so
nothing reads a key back after writing it.
