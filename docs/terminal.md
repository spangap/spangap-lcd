# Terminal — text view and VT100 terminal

The straddle ships two monospace text widgets for on-device output, both usable
by any app showing growing or interactive text. Both are declared in
[lcd.h](../esp-idf/include/lcd.h) (text view) and `lcd_term.h` (terminal), and
both run on the lcd task. Maintainer detail is in
[terminal-internals.md](terminal-internals.md).

## When to use which

- **`lcdTextView`** — an append-only, virtualized scrollback view. It holds an
  arbitrarily large history but only ever lays out the on-screen window, so
  append and scroll cost O(visible rows), not O(scrollback). Use it for growing
  log-style output. The built-in **Log** app is one.
- **`lcd_term`** — a real VT100/xterm terminal backed by libvterm: a fixed
  rows×cols cell grid with cursor addressing, so full-screen apps (top, vim,
  anything ncurses) lay out correctly over a pty. Use it for interactive
  sessions. The built-in **CLI** app is one.

Both are wired to the device over ITS (the Log/CLI tasks' DC ports) — see
[apps-internals.md](apps-internals.md#5-built-in-apps-apps).

## The text view

```cpp
lcd_textview_t* v = lcdTextViewCreate(parent, w, h,
                                      &lv_font_spleen_5x8, fg, budget);
lcdTextViewAppend(v, data, len);   // append; stays pinned to the bottom if it was
```

- Scrollback is capped to `budget` bytes (oldest whole lines trimmed off the top).
- `lcdTextViewSet` replaces the whole scrollback; `lcdTextViewSetSuffix` sets a
  transient tail that is rendered but never stored or trimmed (e.g. a CLI's
  in-progress input line + cursor).
- `lcdTextViewScrollToBottom` / `lcdTextViewAtBottom` manage the reading position;
  appending while pinned to the bottom follows the newest content, otherwise the
  on-screen position is held steady across trims.
- `lcdTextViewSetLineColor(v, cb)` installs a per-line colour callback: given one
  logical line, return `0xRRGGBB` or `LCD_TEXTVIEW_DEFAULT`. Colour is applied at
  render time over plain-text scrollback, so the column math never sees it — this
  is how Log paints per-severity colour without storing escapes.
- `lcdTextViewObj(v)` is the scroll container — add it to `lcdInputGroup()` for
  keyboard focus or hang your own event callbacks on it.

It frees itself when its container is deleted (e.g. when an app's layer is
reclaimed); `lcdTextViewDelete` is optional.

## The VT100 terminal

```cpp
lcd_term_t* t = lcdTermCreate(parent, w, h, &lv_font_spleen_5x8, fg, onOutput, user);
lcdTermFeed(t, bytes, len);   // device->screen VT byte stream; repaints damaged rows
lcdTermKey(t, lvKey);         // one focused key; encoded and emitted via onOutput
```

- rows/cols are derived from the monospace font metrics.
- `onOutput(data, len, user)` receives bytes the terminal needs to send upstream
  (keystroke encodings, query replies) — wire it to your ITS send.
- `lcdTermSize(t, &rows, &cols)` reports the grid for a pty window-change.
- `lcdTermObj(t)` is the focus/click target.
- SGR colours from the application render, including backgrounds and reverse
  video. Drag the body up/down to scroll back through history; any keypress (and
  new output while at the bottom) snaps back to the live screen.
- `lcdTermDestroy(t)` frees the VTerm (the LVGL objects go with the parent layer).

The keyboard driver may OR `LCD_KEY_CTRL` (`lcd.h`) into a key code to mean
"Ctrl+letter"; the terminal decodes it to a control byte.
