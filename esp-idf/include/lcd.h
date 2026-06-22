/**
 * lcd — phone-style LVGL launcher + display owner.
 *
 * Owns the display + input devices and the one LVGL context. LVGL is not
 * thread-safe, so everything that touches it runs on the lcd task: use
 * lcdRun()/ON_LCD to hop onto it (mirrors storage's ON_CHANGE) — that is the
 * only place LVGL may be touched.
 *
 * Layering (bottom -> top): launcher (program icons) | per-program layers |
 * opaque status bar. A component registers a program with lcdRegister(); when
 * its icon is opened, lcd creates (or re-shows) that program's own layer and
 * calls the registered fn with the layer as its argument. The fn must touch
 * nothing outside that layer. A swipe up from the bottom edge (or lcdGoHome())
 * hides the layer back to the launcher; layers persist, so re-opening resumes
 * where it left off.
 *
 * Gated on CONFIG_SPANGAP_LCD. lcdInit() is called by spangapInit(). The display
 * is configured through Kconfig (CONFIG_LCD_*); a consumer supplies only input
 * via the lcd_input.h contract.
 */
#ifndef SPANGAP_LCD_H
#define SPANGAP_LCD_H

#include "lvgl.h"

/** Callback run on the lcd task. `arg` is reserved for lcdRun() (defaults
 *  null) and is the program's layer object (lv_obj_t*) for lcdRegister(). */
typedef void (*lcd_fn_t)(void* arg);

/** Lambda -> lcd_fn_t sugar, mirroring storage's ON_CHANGE. No captures. */
#define ON_LCD [](void* arg)

/** Modifier bit a board keyboard driver OR's into an lv key to mean "Ctrl +
 *  this (lowercase) letter". Sits above every LVGL key code and ASCII, so it
 *  never collides; the on-device terminal decodes it to a control byte. */
#define LCD_KEY_CTRL 0x40000000u

/** Bring up display, LVGL, launcher and status bar, and spawn the lcd task.
 *  Called by spangapInit() when CONFIG_SPANGAP_LCD=y. Safe to call once. */
void lcdInit(void);

/** Run fn(arg) on the lcd task — the only place LVGL may be touched. Returns
 *  once queued; fn runs shortly after on the lcd task. Safe from any task. */
void lcdRun(lcd_fn_t fn, void* arg = nullptr);

/** Register a launcher program. `name` is the icon label; `iconBasename`
 *  names a file under /fixed/lcd/icons/<res>/<iconBasename>.bin (no extension,
 *  no size — <res> is fixed to the launcher tile size, "36x36"; icons render at
 *  that native size). `fn` runs on the lcd task with
 *  the program's layer (lv_obj_t*) the first time the icon is opened, or again
 *  if the layer was reclaimed. Safe to call from any task / any time. */
/** `onShow` (optional) is a per-program "shown" callback, invoked every time the
 *  program's layer is brought to the front (tile tap OR lcdShowProgram), AFTER it
 *  is built and focus restored — distinct from the build-once `fn`. Lets a
 *  program react to being opened (e.g. the viewer navigating to its home page on
 *  a manual launch). Both run on the lcd task. */
void lcdRegister(const char* name, const char* iconBasename, lcd_fn_t fn,
                 lcd_fn_t onShow = nullptr);

/** Bring a registered program's layer to the front (building it on first use,
 *  exactly as a tile tap would) by its registered `name`. No-op if no program
 *  with that name is registered. Must run on the lcd task — call from a
 *  registered fn, an lcdRun() callback, or another lcd-task handler. */
void lcdShowProgram(const char* name);

/** Set backlight 0..255 (0 = off). Persists s.lcd.backlight. Any task. */
void lcdSetBacklight(uint8_t level);

/** Report user input to the inactivity tracker (e.g. a consumer hardware-keyboard
 *  keystroke — the touch/button/trackball indevs report themselves). Resets the
 *  blank timer and, if the screen is in standby, wakes it. Returns true iff this
 *  call woke the screen, so an on-lcd-task caller can swallow the waking key
 *  rather than deliver it. Safe from any task (off-task callers always get false
 *  and the wake is posted asynchronously). */
bool lcdNotifyActivity(void);

/** Hide the current program layer and return to the launcher. Runs on the lcd
 *  task; safe to call from a registered fn (e.g. a Back button). */
void lcdGoHome(void);

/** Hide the status bar and grow the current program layer to the full screen
 *  height (`on`), or restore both. For an immersive program screen (e.g. a chat
 *  thread). The launcher remembers which layer asked: the bar stays hidden only
 *  while that layer is the one on screen, comes back when it goes Home, and is
 *  re-hidden when the program is re-opened — so callers just toggle it as their
 *  own view changes. Percentage-sized children of the layer reflow to the new
 *  height automatically. Runs on the lcd task; call from a registered fn. */
void lcdProgramFullscreen(bool on);

/** Program property (mirrors lcdProgramFullscreen): while this program's layer
 *  is the one on screen, the trackball emits arrow keys into the focus group
 *  instead of moving the pointer — so an on-device terminal / vim can navigate.
 *  The launcher turns it off when the layer goes Home and back on when it's
 *  re-shown. Call from a registered fn. */
void lcdProgramScrollwheelArrows(bool on);

/** True while the trackball is in arrow-key mode (see above). The board's
 *  pointer_read consults this to decide whether to move the pointer or feed
 *  arrows to lcdInputGroup(). Lcd task. */
bool lcdScrollwheelArrowsActive(void);

/** The shared keypad focus group that hardware button / keyboard indevs target.
 *  Add focusable widgets you build in a program (e.g. a textarea) with
 *  lv_group_add_obj() so a physical keyboard can type into them. Lcd task. */
lv_group_t* lcdInputGroup(void);

/** Tell the lcd component whether the device has a real text keyboard. The lcd
 *  component owns no keyboard itself (a consumer may provide one as its own
 *  keypad indev joined to lcdInputGroup()); call this so the built-in Settings
 *  text fields edit in place instead of popping an on-screen keyboard. Default
 *  false (no keyboard → on-screen keyboard). Any task. */
void lcdSetHasKeyboard(bool present);

/** Whether a consumer has reported a hardware text keyboard (see
 *  lcdSetHasKeyboard). Lets a program pick a keyboard-friendly layout (edit in
 *  place) over a touch-only one (tap → on-screen keyboard). Any task. */
bool lcdHasKeyboard(void);

/** How long the cursor (driven by a board pointer device) stays visible after
 *  activity, in milliseconds; <0 = always visible. lcd owns the cursor but not
 *  this policy — the consumer that owns the pointing device sets it (e.g. from
 *  its own config key). Runs on / hops to the lcd task. */
void lcdPointerSetVisibleMs(int ms);

/* ---- Multi-touch / gestures ----
 * A consumer that wants raw multi-finger input (e.g. pinch-zoom) enables
 * multipoint reads and registers a gesture handler. Point 0 still drives the
 * normal single-pointer indev (single-finger UI is unaffected); with >=2
 * fingers down the pointer is suppressed so the gesture owner has the gesture.
 * The high-rate finger data flows through the callback, never storage. */

/** One touch point, in display (post-rotation) coordinates. */
typedef struct { int16_t x, y; } lcd_touch_pt_t;

/** Gesture handler, run on the lcd task whenever touch is sampled while
 *  multipoint is enabled. `count` is the number of fingers (0 = all lifted). */
typedef void (*lcd_gesture_cb_t)(const lcd_touch_pt_t* pts, int count);

/** Enable/disable multi-point touch reads. Off by default (single-touch, the
 *  cheapest path); no-op on boards without a multi-point panel. Cheap to
 *  toggle — enable while gestures are wanted, disable when done. Any task. */
void lcdTouchSetMultipoint(bool on);

/** Register a gesture handler (fixed small set). Call once at init. */
void lcdTouchAddGestureHandler(lcd_gesture_cb_t cb);

/* ---- Fonts ----
 * Bundled LVGL fonts a program may set on its own widgets with
 * lv_obj_set_style_text_font(). */

/** Montserrat 12px, 4bpp — a drop-in accented superset of LVGL's stock
 *  lv_font_montserrat_12 (which is ASCII + symbols only). Adds Latin-1
 *  Supplement + Latin Extended-A (U+00A0–U+017F: Western & Central European
 *  accents) while keeping the full LVGL symbol set, so it renders user text
 *  (names, messages) without dropping umlauts/accents to placeholder boxes.
 *  Always compiled into LCD builds; regenerated by scripts/gen-text-font.py. */
extern const lv_font_t lv_font_montserrat_12_latin;

/** Montserrat 16px, 4bpp — a larger accented companion to the 12px face above
 *  (ASCII + Latin-1 Supplement + Latin Extended-A; no symbol set). For headings
 *  and other larger proportional text. Always compiled into LCD builds;
 *  generated with lv_font_conv over Montserrat-Medium.ttf. */
extern const lv_font_t lv_font_montserrat_16_latin;

/** Spleen 5×8 — the platform's fixed-width terminal font, used by the on-device
 *  Log / CLI text views (see lcdTextViewCreate). 2 bpp greyscale. Spleen's text
 *  glyphs merged with misc-fixed 5×8 for broad coverage: the COMPLETE Box
 *  Drawing block U+2500–257F (heavy/double lines and half-strokes included)
 *  plus synthesized Block Elements U+2580–259F — the 5 px cell halves at
 *  2.5 px, so half-cell blocks (halves, quadrants) put a grey pixel on the
 *  center column; shades ░▒▓ are flat greys. No LVGL symbols. Use it for
 *  monospace / column-aligned content (ASCII art, tables, box-drawn frames)
 *  where glyph widths must match; use lv_font_montserrat_12_latin for chrome
 *  needing symbols. Always compiled into LCD builds; regenerated by
 *  scripts/gen-spleen-font.py. */
extern const lv_font_t lv_font_spleen_5x8;

/** Tom Thumb 4×6 — the smallest monospace font (80 columns on a 320 px panel),
 *  with near-complete "terminal graphics" coverage: Tom Thumb's 3×5-in-4×6 text
 *  glyphs (ASCII + Latin-1) merged with misc-fixed 4×6 for everything else —
 *  notably the COMPLETE Box Drawing block U+2500–257F (heavy lines and
 *  half-strokes included) and all Block Elements U+2580–259F (the quadrants
 *  U+2596–259F are synthesized). The even cell width makes 2×2 block glyphs
 *  tile seamlessly across cells, so micron/NomadNet page "graphics" connect.
 *  Always compiled into LCD builds; regenerated by scripts/gen-tomthumb-font.py. */
extern const lv_font_t lv_font_tomthumb_4x6;

/** Micro 2×3 — the page-THUMBNAIL font (160 columns on a 320 px panel), 2 bpp.
 *  Deliberately unreadable: every glyph of the Tom Thumb 4×6 merged set
 *  box-filtered 2× to a 2×3 cell, so a whole oversized page (micron "graphics"
 *  are often drawn 100+ columns wide) fits on screen as a layout preview.
 *  Block elements survive exactly (a quadrant becomes one full pixel); box
 *  lines and text become grey traces/texture. Same coverage as
 *  lv_font_tomthumb_4x6 by construction. Always compiled into LCD builds;
 *  regenerated by scripts/gen-micro-font.py. */
extern const lv_font_t lv_font_micro_2x3;

/* ---- Settings ---- */

/** Register an entry in the built-in Settings menu (the gear program lcd owns).
 *  `path` is a slash-path whose intermediate segments become submenus
 *  (auto-created, title-cased), e.g. "net/wifi" -> a "Net" submenu with a
 *  "Wi-Fi" item. `label` names the leaf item; `fn` is called on the lcd task
 *  with the item's content widget (an empty, scrollable flex-column container —
 *  build the pane into it with the lcdSetting* helpers). Call at init, before
 *  Settings is first opened; populates a registry, so it works even before
 *  lcdInit() and from any init task.
 *  `placement` orders the leaf among its siblings (and likewise menus among
 *  menus): > 0 toward the top, ascending (1 is topmost); 0 (default) in the
 *  middle, alphabetic; < 0 toward the bottom, ascending (-1 is bottom-most).
 *  Ties sort alphabetically by label. */
void lcdRegisterSettings(const char* path, const char* label, lcd_fn_t fn, int placement = 0);

/* ---- Setting-pane helpers ----
 * Build a uniform labeled row in `parent` (a Settings pane widget) bound to a
 * storage key, mirroring the browser's Setting* components. Run on the lcd
 * task (inside a settings fn). Each returns the row object. */

/** Bold section divider (no control). */
lv_obj_t* lcdSettingSection (lv_obj_t* parent, const char* title);
/** Greyed, wrapped help text under a control (mirrors the browser's captions). */
lv_obj_t* lcdSettingCaption (lv_obj_t* parent, const char* text);
/** Toggle bound to an int key (0/1). */
lv_obj_t* lcdSettingSwitch  (lv_obj_t* parent, const char* label, const char* key);
/** Slider bound to an int key, clamped to [min,max]. */
lv_obj_t* lcdSettingSlider  (lv_obj_t* parent, const char* label, const char* key, int min, int max);
/** Text field bound to a string key; tap opens an on-screen keyboard. When
 *  `secret`, the value is masked. */
lv_obj_t* lcdSettingText    (lv_obj_t* parent, const char* label, const char* key, bool secret = false);
/** Dropdown bound to a string key; `optionsCsv` is a comma-separated list. */
lv_obj_t* lcdSettingDropdown(lv_obj_t* parent, const char* label, const char* key, const char* optionsCsv);
/** Read-only row that live-updates from a string key (auto-refreshes ~1 Hz). */
lv_obj_t* lcdSettingValue   (lv_obj_t* parent, const char* label, const char* key);
/** Action button; `onClick` runs on the lcd task (arg is the row). */
lv_obj_t* lcdSettingButton  (lv_obj_t* parent, const char* label, lcd_fn_t onClick);

/* ---- Virtualized text view ----
 * A vertically-scrolling monospace text view that holds a large scrollback but
 * only ever lays out the on-screen window into LVGL — append and scroll cost
 * O(visible rows), not O(scrollback), however deep the history. Built for the
 * on-device Log / CLI terminals; usable by any program showing growing or large
 * text. All calls run on the lcd task (LVGL is single-threaded). */

typedef struct lcd_textview_t lcd_textview_t;

/** Create a text view filling a `w`x`h` box in `parent`, rendered in monospace
 *  `font` / colour `fg`, scrollback capped to `budget` bytes (oldest whole lines
 *  trimmed). Freed automatically when its container (a child of `parent`) is
 *  deleted — e.g. when a program layer is reclaimed. */
lcd_textview_t* lcdTextViewCreate(lv_obj_t* parent, int32_t w, int32_t h,
                                  const lv_font_t* font, lv_color_t fg, size_t budget);

/** Append text to the scrollback. Stays pinned to the bottom if it already was;
 *  otherwise the on-screen reading position is held steady across trims. */
void lcdTextViewAppend(lcd_textview_t* v, const char* data, size_t len);

/** Replace the entire scrollback with `data`. */
void lcdTextViewSet(lcd_textview_t* v, const char* data, size_t len);

/** Set a transient tail rendered after the scrollback but never stored or
 *  trimmed (e.g. a CLI's in-progress input line + cursor). null/0 clears it. */
void lcdTextViewSetSuffix(lcd_textview_t* v, const char* data, size_t len);

/** Jump to and pin the newest content (the bottom). */
void lcdTextViewScrollToBottom(lcd_textview_t* v);

/** True while the view sits at the bottom (newest content visible). */
bool lcdTextViewAtBottom(lcd_textview_t* v);

/** The scroll container — add it to lcdInputGroup() for keyboard focus, or
 *  attach your own event callbacks (e.g. a CLI line editor on LV_EVENT_KEY). */
lv_obj_t* lcdTextViewObj(lcd_textview_t* v);

/** Per-line colour callback: given one logical line of the scrollback (up to,
 *  not including, its newline), return 0xRRGGBB — or LCD_TEXTVIEW_DEFAULT to
 *  use the view's fg. Called at render time for the visible band only, so it
 *  must be cheap and pure (same line → same colour). */
#define LCD_TEXTVIEW_DEFAULT 0xFFFFFFFFu
typedef uint32_t (*lcd_textview_line_color_cb)(const char* line, size_t len);

/** Install (or clear, with NULL) the per-line colour callback and repaint.
 *  The view stores plain text either way — colour is applied at render. */
void lcdTextViewSetLineColor(lcd_textview_t* v, lcd_textview_line_color_cb cb);

/** Destroy the view (deletes its container). Optional — deleting the parent
 *  layer frees it automatically. */
void lcdTextViewDelete(lcd_textview_t* v);

#endif /* SPANGAP_LCD_H */
