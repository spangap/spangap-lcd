# Settings — the on-device settings menu

The shell installs a built-in **Settings** app (the gear tile) that hosts a
nested menu of storage-bound panes — the on-device counterpart of the browser's
settings panels. Any straddle registers a pane with a slash-path and builds it
from the `lcdSetting*` helpers; the platform's net straddle, for example, adds a
`Net/Wifi` pane. Maintainer detail is in [settings-internals.md](settings-internals.md).

## Registering a pane

```cpp
#include "lcd.h"

lcdRegisterSettings("Net/Wifi", "Wifi", ON_LCD {
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

- **`path`** is a slash-path whose intermediate segments auto-become submenus
  (title-cased), e.g. `"Net/Wifi"` creates a **Net** submenu containing a
  **Wifi** item. Segments are matched **case-sensitively by their raw id**, so
  two straddles sharing a submenu must spell the segment identically —
  first-letter-uppercase (`Net/...`) is the platform convention.
- **`label`** names the leaf item.
- **`fn`** is called on the lcd task with the item's content pane (an empty
  scrollable flex column); build it with the helpers below.
- An optional **`placement`** orders the leaf among its siblings: `> 0` toward
  the top (ascending, `1` topmost), `0` (default) in the middle alphabetic,
  `< 0` toward the bottom. Ties sort alphabetically by label.

Call at init, from any task — it populates an in-RAM registry, so it works even
before `lcdInit()`. (Other straddles' panes run from their `*LcdRegister` init
hooks, guarded on `CONFIG_SPANGAP_LCD`.)

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
