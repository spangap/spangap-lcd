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
    { .id = "net",     .label = "WiFi & Network", .shortName = "Network", .order = 10, .has_order = true },
    { .id = "wifi",    .label = "WiFi",           .shortName = "WiFi",    .order = 1,  .has_order = true },
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
  **`shortName`** is the header text and defaults to the label. The **last**
  contributor to supply each wins, so the buildable — which contributes after
  every straddle it stages — has the final say. By convention a node is named
  once, by the straddle it exists for, and everybody else states only its `id`.
- **`order`** places a node among its siblings. One rule everywhere: nodes
  carrying an order first, ascending; the rest after them in contribution order
  (which is init order — the platform's nodes land before a consumer's).
- **`fn`** is called on the lcd task with the node's content pane (an empty
  scrollable flex column); it may be null for a contribution that only names
  nodes. A node left with no rows and no rendering descendant gets no navigation
  row — naming a menu does not by itself put one on the screen.

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
| `lcdSettingTitle(parent, title)` | the pane's own name: white, the largest type on it, under a hairline rule. Deliberately NOT the accent colour — the accent marks a divider within a page, and a page's name is not one of its dividers | — |
| `lcdSettingHeading(parent, title)` | a group heading: the accent colour, bold, half again the body size | — |
| `lcdSettingSection(parent, title)` | a sub-group inside a group: the same accent a size smaller, since the display has no indentation to tell the levels apart with | — |
| `lcdSettingCaption(parent, text, underHeading=false)` | greyed, wrapped help text. `underHeading` spans both columns at the heading's indent; otherwise it starts on the control column, where the control it describes starts. `[label](url)` keeps the label and drops the URL — a panel with no browser has nowhere to send you | — |
| `lcdSettingSwitch(parent, label, key)` | toggle | int key (0/1) |
| `lcdSettingSlider(parent, label, key, min, max)` | slider, with a live mono readout right-aligned in a fixed six-digit column at the row's right edge, clamped | int key |
| `lcdSettingInteger(parent, label, key, spec)` | a number typed in: digits only, an optional `-`/`+` pair snapping to multiples of `spec->step`, and a warning modal on a value outside `spec`'s bounds (refused, not clamped) | int key |
| `lcdSettingText(parent, label, key, secret=false, unit=null, narrow=false)` | text field (inline edit or on-screen keyboard). `unit` is a word printed after it — never part of the value; a field carrying one is narrow and right-aligned | string key |
| `lcdSettingIpv4(parent, label, key)` | dotted quad: digits and dots only, four octets of 0-255 on commit, and a warning modal on anything else. Empty is accepted and means unset | string key |
| `lcdSettingDropdown(parent, label, key, optionsCsv)` | dropdown, sized to its longest option rather than stretched across the column | string key |
| `lcdSettingValue(parent, label, key)` | read-only, live | string key |
| `lcdSettingButton(parent, label, onClick)` | action button | — (`onClick` on lcd task) |
| `lcdSettingWhenKey(row, key)` | wraps any row above; shows it while the key is truthy | string key |
| `lcdSettingAction(parent, label, act, color)` | button running a descriptor action, in the control column | — |
| `lcdSettingActionRow(parent, align, btns, n)` | several content-sized buttons sharing the control column, gathered at `align` | — |
| `lcdSettingInfo/InfoValue/InfoFit` | a block of read-only values, own label column | string keys |
| `lcdSettingCollection(parent, desc)` | an array, as an editable list: alternating dark-grey bands, its text a step under the body size | the descriptor's array key |

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

## Buttons that share a row

Both button helpers start on the CONTROL column, where every field on the pane
starts: a button is a control, and one beginning a third of the pane further
left reads as belonging to none of the rows around it.

`lcdSettingAction` fills that column, which is right for a single action and
wrong for two that are one choice. `lcdSettingActionRow` puts several
`lcd_btn_t` on one line inside it, each sized to its label, gathered
`LCD_ALIGN_LEFT` (the default in the
yaml), `_CENTER` or `_RIGHT`. A button may carry a `when_key` of its own, gating
that button rather than the line — a hidden flex child leaves the layout and the
rest close up around it. The row wraps rather than clipping, so a pair whose
labels are too wide for the display stacks instead of running off it.

Every button, here and in a dialog and on a collection's item rows, may state a
`color`: `red`, `green`, `amber`, `blue`, `grey` or an explicit `rrggbb`, from
the same palette and the same table a status pill uses. Null is the button's own
colour, and is what almost every button should be.

## Blocks of read-only values

A run of value rows is a readout, and the ordinary row layout — a third of the
pane for the label, whatever the pane's longest label needs — leaves it full of
gaps. An **info group** gives that run its own label column: sized to the widest
label *in the group* and never wider than the third an ordinary row uses, with
no gap between the lines.

```cpp
lv_obj_t* g = lcdSettingInfo(pane);
lcdSettingInfoValue(g, "Status", "wifi.sta.state_text");
lcdSettingWhenKey(lcdSettingInfoValue(g, "IP", "wifi.sta.ip"), "wifi.sta.up");
lcdSettingInfoFit(g);
```

Three calls rather than one because the column is only knowable once every label
exists — `lcdSettingInfoFit` is what says the run has ended. It measures every
line, hidden ones included, so a `when_key` line appearing never shifts the
column. Read-only values only: a control needs room the narrow column does not
give it, so anything interactive is an ordinary row above or below the group.
A long value ellipsizes here (and marquees on focus) rather than taking the
stacked shape a lone `lcdSettingValue` falls back to — the stack has no left
column, and a group whose lines disagreed about that would not be one.

The group carries no heading: `lcdSettingSection` above it is the heading, which
also lets several groups sit under one.

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

  Every one of these — dialogs, forms, the on-screen keyboard, the long-value
  editor — registers with `lcdModalTrack()`, so **a double tap anywhere closes
  it** whatever its own buttons are doing (see [shell](shell.md#getting-out-of-a-dialog)).
  The form and the two editors pass their own closer, because the escape has to
  drop their subscriptions and must never commit a half-typed value.
- **form** — the one dialog with inputs, because it fronts a sentinel. Fields
  are ordinary rows carrying `field` instead of `key`; values live in a local
  buffer and reach the device as one JSON object on submit. On a board with a
  physical keyboard the fields are inline textareas, otherwise tapping one opens
  the on-screen keyboard — one form serves both paths, so nothing branches on
  `lcdHasKeyboard()` any more.

  **Nothing a form shows is on the device until Save.** Typing fills the local
  buffer and no more; Save serializes it to the sentinel and the owning task
  either rejects it (a sentence appears, the form stays open) or accepts it (the
  form closes). Cancel discards the buffer. That is the whole reason a form is
  the only dialog with inputs — a pane row writes its key the instant you touch
  it, and a set of fields that must be judged together cannot.

  A form's rows are compact ones (three quarters of a pane row's height) at a
  step below the pane's body size, and everything under the title — fields, the
  rejection line, and the buttons — is in one scroll container. The buttons are
  sized to their labels and gathered right: destructive first, then Cancel, then
  the submit. Nothing is pinned; on this panel a fixed footer costs more than
  scrolling past it does.

  A `section:` or `caption:` inside a form templates over the field buffer and
  follows it as it is edited. In an **item editor** a name the form has no field
  for falls back to the item's stored value, which is what lets a caption talk
  about a field the page does not offer — leaving a field out of `edit:` is how
  a page says "not this", and the handler carries it forward untouched. A text field's `placeholder_key` shows a hint the device
  publishes — the MAC it would use if the field is left blank.

A **collection** (`lcd_collection_t`) renders the array at its `key` as banded
rows — two alternating dark greys, so a block of the device's own data is
visibly not more furniture — each carrying a title, an optional subtitle, a
status pill, and, with `reorder`, a grip at the right edge.

The grip is what a **drag** starts on, and only there: a vertical drag anywhere
else on a list is the pane scrolling, which is what a finger on a long pane is
nearly always doing. It refuses to chain its scroll to the page, so a press on
it starts a drag instead. While dragging, only the dragged row moves — it is
translated under the finger and drawn over its neighbours; the release writes
the whole id order to `<cmd>.order` and the re-published array is what redraws
the list.

**That is all a row carries** — no chevron either: a banded block inside a pane
of chevron-less rows already reads as a list of things, and the affordance would
cost width the title needs. Everything that acts on the item — the `edit` rows,
the per-item `actions`, removal — is on the item's **detail page**, which tapping
anywhere on the row opens. It is the `edit` form with the item's buttons added to
its foot: Delete (red) first, then each action, then Cancel and Save. The page is
headed by the ITEM's own title — the collection's `item:` template substituted
over the item being opened — because the collection's name over it ("Known
networks") says nothing about *which* item. So an `edit:` block states no heading
of its own: a collection names its detail page by having named its rows. An action or a removal closes the page before
it runs, so a confirmation lands on a clear screen rather than on top of the page
it is about to invalidate. Five buttons on a 320 px row is a row nobody can hit;
reordering stays on the row because it is about the row's place, not the item,
and needs its neighbours in view.

The collection's **own** buttons — a `candidates` scan and each `add:` — share
one bar under the list, sized to their labels and gathered right, scanning
first.

**Scan results are a popup**, not part of the pane — the same on both surfaces. What the device can *see* is
a different question from what it is configured for, and a transient answer to
it: results arrive over seconds, they change, and they are gone the moment you
stop asking. In the pane they would push the configured list around while you
read it, and land under a button that may be most of a screen below the fold.
As a popup they are the whole screen and they start at the top of it. It is
headed by the clause's `found:` text — what is on screen, not the button that
opened it — with a small Close in the top-right corner rather than a button row
beneath, which on a card that is one long list would just be another row of it.
The list scrolls inside the card; picking one closes the popup and opens the add
form prefilled. Opening it starts the scan and closing it stops the scan, which
is the whole "stop scanning on leave" contract — the same key the pane's
teardown clears.

An action carries an optional `when_key`, templated over the item to build a
key (`"wifi.netjoinable.{id}"`) and subscribed live, which is how "Connect"
disappears from the network already connected. Truthiness only: the owning task
publishes the gate, so no UI compares anything.

A collection **never writes the array**: it writes `<cmd>.add`, `<cmd>.remove`,
`<cmd>.set` and `<cmd>.order`, and the owning task is the array's only writer. A
reorder sends the complete id order comma-joined, which the firmware applies as
a *preference permutation* (recognized ids into that relative order, unknown ids
ignored, unmentioned ids left in place) — idempotent, and harmless against a
concurrent edit.

A `candidates` clause adds scan-and-adopt: an ephemeral array the task
publishes, rendered like the item rows, where picking one opens the first add
form prefilled. Leaving the pane clears the refresh target key, so nothing has
to carry a "stop scanning on leave" timer.

Everything here is event-driven — callbacks and storage subscriptions, no waits
and no polling. `storageSet` is async (it queues to the owning actor), so
nothing reads a key back after writing it.
