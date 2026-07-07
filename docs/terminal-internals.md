# Terminal — internals

Maintainer reference for the text view (`lcd_textview.cpp`), the libvterm-backed
terminal (`lcd_term.cpp` + the vendored `libvterm/`), and the terminal fonts.
The [operator guide](terminal.md) is the consumer view. Both widgets run
on the lcd task (LVGL is single-threaded).

## 1. Origins

`libvterm` is the neovim mirror (MIT), vendored under
[`esp-idf/libvterm/`](../esp-idf/libvterm) and compiled straight into the
component (plain C, no autoconf). Its public header is private to us — only
`lcd_term.cpp` includes it — and warning flags are suppressed on its sources
(`-w`) since we don't maintain upstream. The component allocates libvterm's
~130 KB screen (two cell buffers at ~64×26) in PSRAM via a custom
`VTermAllocatorFunctions`; libvterm requires zeroed memory, so the allocator uses
`heap_caps_calloc(MALLOC_CAP_SPIRAM)`.

## 2. The text view (lcd_textview.cpp)

Holds the scrollback in a `std::string` but lays out only the on-screen window
into pooled LVGL labels, so append and scroll are O(visible rows). An invisible
spacer child gives the scroll container its true content height (rows × pitch),
so LVGL's own scrollbar + scroll physics work unchanged, while the band of labels
is repositioned and refilled around the current offset.

- **Monospace is assumed** for the column math: text is hard-wrapped at a fixed
  column count (terminal-style, not word-wrapped) and the row pitch is the font
  line height with zero line spacing, so the spacer height stays exact and
  scrolling never drifts.
- **Double-buffered bands.** Two band containers (front shown, back built), each
  with a pooled label per run of consecutive same-colour rows — with no line
  colorizer that's one label per band (the original single-label design).
- **Debounced rendering.** The panel flush (a few Hz) is the bottleneck, so the
  win is *skipping* intermediate draws, not rate-limiting them. Append/set/scroll
  only mark the view dirty and (re)arm a paused `lv_timer` that fires once
  activity settles for `SETTLE_MS`, doing one reflow + repaint of the latest
  state and re-pausing. A `MAX_DEFER_MS` cap bounds the wait so a non-stop stream
  still shows progress. The timer pausing when clean keeps the lcd task's
  light-sleep loop intact (no idle render wake).

## 3. The terminal (lcd_term.cpp)

libvterm owns the VT state machine and the rows×cols live screen; the widget
renders a window over `[scrollback ++ live screen]` into per-row label runs.

- **Cell metrics from the font.** `charW` is the glyph *advance*
  (`lv_font_get_glyph_width(font, 'M', 0)`), not a bbox, and `rowH` the font
  line height, so a vector `MONO` face at any size yields a correct grid;
  `cols`/`rows` divide the widget box by them.
- **Run-label rendering.** Each visible row is painted as a row of labels, one
  per run of cells sharing `(fg, bg)`. Run labels (not LVGL's "recolor") are used
  deliberately: LVGL's recolor `#` escape is broken in 9.5 (`lv_text_is_cmd`'s
  escape branch is dead code) and terminal text is full of literal `#`, and runs
  also get backgrounds, which recolor can't do. Row label pools are reused across
  repaints; extras are hidden. The cursor block is a separate object kept
  composited above the run labels.
- **Colour resolution.** `cellColors` resolves each cell to 24-bit (or the
  `TERM_DEFAULT` sentinel = widget fg / transparent bg), applying reverse video
  by swapping fg/bg (defaults resolve to concrete colours under a reverse swap).
- **Scrollback codec.** Lines that scroll off the top (`sb_pushline`) are kept in
  a PSRAM ring (`std::deque<std::string>`, `SB_CAP` = 600 lines). Each line is
  encoded compactly: per run a `0x1F` marker + fg + bg + cols + text, one
  `std::string` per line, so colour survives scrolling without a vector-per-line
  heap footprint (`0x1F` can't occur in cell text). Trailing blanks are trimmed
  except where the run has a coloured background (visible ink).
- **Following vs scrolled-back.** `follow` sticks the window to the live screen;
  dragging the body sets `top` and clears follow; any key (and `lcdTermFeed` while
  following) snaps back. The cursor shows only on the live screen when following.
- **Damage merge.** `lcdTermFeed` writes input, flushes merged damage
  (`VTERM_DAMAGE_SCROLL`) to the callbacks, and re-renders only if following.
- **Key encoding.** `lcdTermKey` maps LVGL key codes to `vterm_keyboard_key`
  (enter/backspace/esc/arrows) or `vterm_keyboard_unichar` for printables;
  `LCD_KEY_CTRL` (`lcd.h`) → `VTERM_MOD_CTRL` + the low byte.

`SCREEN_CBS` is static (libvterm stores the pointer, so it must outlive the
screen); it is zero-initialised so unset callbacks stay NULL.

## 4. Fonts

Terminal-shaped content resolves its font through `lcdFont(LcdFace::MONO, px)`
(see shell-internals §9 for the engine): 5–8 px requests map to the bitmap
fonts below, anything larger renders the DejaVu Sans Mono vector face, whose
build-time subset (`scripts/lcd-fonts.py`) carries Latin-1, the complete Box
Drawing block U+2500–257F, all Block Elements U+2580–259F, the TUI-common
Geometric Shapes U+25A0–25CF, and arrows U+2190–2193 — so box art and Micron
"graphics" have glyphs at every size. `MONO_BOLD`/`MONO_ITALIC` are synthesized
from the same file under FreeType (and degrade to plain `MONO` under tiny_ttf).

**Known pitfall — box-drawing seams at vector sizes.** A cell grid tiled from
rasterized outline glyphs can show 1 px seams between adjacent box/block glyphs
at unlucky (size, hinting) combinations — the reason kitty/alacritty draw these
characters procedurally. Nothing procedural exists here: the widgets render
whatever glyphs the font gives them. If seams show at the sizes people actually
use, the sanctioned fix is to intercept U+2500–259F in the term/Micron draw
path and draw rects/fills to exact cell bounds — pixel-perfect at any size, and
it removes the coverage requirement from font choice entirely. Until then,
inspect box art when changing the mono face or its subset flags (the subset
keeps TrueType hints partly for this).

The compiled-in bitmap fonts are checked-in generated `.c` arrays under
`src/lcd_ui/`, declared in `lcd.h`; an ordinary build needs no tooling. The
monospace ones are the 5–8 px `MONO` band and the `LCD_FONT_BITMAP` engine's
targets, and all remain directly usable for fixed-cell content. Regenerate with
the matching `scripts/gen-*.py` and commit the `.c`. Source fonts are OFL-1.1
(Font Awesome Free additionally CC-BY-4.0).

- **`lv_font_spleen_5x8`** — the terminal font, 2 bpp greyscale. Spleen's text
  glyphs (BSD-2-Clause) merged with X11 misc-fixed 5×8 (public domain) for
  coverage by `gen-spleen-font.py`: the complete Box Drawing block comes from
  misc-fixed (its light lines match Spleen's geometry), and Block Elements
  U+2580–259F are synthesized from per-pixel coverage — the odd 5 px width halves
  at 2.5 px, so half-cell blocks put a grey pixel on the centre column and the
  shades ░▒▓ map to flat greys.
- **`lv_font_tomthumb_4x6`** — the smallest monospace (80 columns at 320 px). Tom
  Thumb's 3×5-in-4×6 glyphs (MIT) merged with misc-fixed 4×6 (public domain) by
  `gen-tomthumb-font.py`; complete Box Drawing + all Block Elements (the
  quadrants U+2596–259F synthesized). The even cell width makes 2×2 block glyphs
  tile seamlessly, so micron/NomadNet page graphics connect — Spleen's odd cell
  can't.
- **`lv_font_micro_2x3`** — the deliberately unreadable page-thumbnail font (160
  columns, 2 bpp): the Tom Thumb 4×6 merged set box-filtered 2× to a 2×3 cell, by
  `gen-micro-font.py` (it imports the Tom Thumb generator for parsing/merge).
  Block elements survive exactly; text becomes grey texture. Re-run it whenever
  the 4×6 sources change.
- **`lv_font_montserrat_12_latin`** / **`lv_font_montserrat_16_latin`** — the
  proportional chrome fonts, accented supersets of LVGL's Montserrat (Latin-1
  Supplement + Latin Extended-A), by `gen-text-font.py` over the Montserrat +
  FontAwesome TTFs that ship in the lvgl component. Text is UTF-8 end to end;
  LVGL decodes each sequence to a codepoint and looks the glyph up by codepoint,
  so the font only has to *carry* that codepoint (which the range above ensures).
  The 12px keeps the full `LV_SYMBOL_*` set; the 16px has no symbol set.

## 5. Pitfalls

- **Don't use LVGL label recolor for terminal/log text** — its `#` escape is dead
  code in 9.5 and collides with literal `#`; use run labels (§3).
- **libvterm needs zeroed allocations** — keep the `calloc`-based allocator; a
  plain `malloc` corrupts the screen.
- **`SCREEN_CBS` must outlive the VTerm** — keep it static; libvterm stores the
  pointer.
- **The text view assumes a monospace font** — a proportional font breaks the
  column/spacer math.
- **Box/block glyphs can seam between cells at vector sizes** — inspect box art
  when touching the mono face or subset; the procedural-drawing contingency is
  §4.
