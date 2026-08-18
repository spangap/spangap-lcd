/**
 * lcd — phone-style LVGL launcher + display owner.
 *
 * Owns the display + input devices and the one LVGL context. LVGL is not
 * thread-safe, so everything that touches it runs on the lcd task: use
 * lcdRun()/ON_LCD to hop onto it (mirrors storage's ON_CHANGE) — that is the
 * only place LVGL may be touched.
 *
 * Layering (bottom -> top): launcher (program icons) | per-program layers |
 * opaque status bar. A component contributes a program by handing an LcdApp
 * (lcd_app.h) to lcdInstall(); opening its tile creates (or re-shows) that app's
 * own layer, which is the only thing it may touch. A swipe up from the bottom
 * edge (or lcdGoHome()) hides the layer back to the launcher; layers persist, so
 * re-opening resumes where it left off.
 *
 * Gated on CONFIG_SPANGAP_LCD. lcdInit() is called by spangapInit(). The display
 * is configured through Kconfig (CONFIG_LCD_*); a consumer supplies only input
 * via the lcd_input.h contract.
 */
#ifndef SPANGAP_LCD_H
#define SPANGAP_LCD_H

#include "lvgl.h"
#include "lcd_settings_desc.h"

/** Callback run on the lcd task. `arg` is whatever the lcdRun() caller passed
 *  (defaults null). */
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

/* Launcher programs are LcdApp objects (lcd_app.h): subclass LcdApp and hand an
 * instance to lcdInstall(). The free functions below act on whichever app is in
 * the foreground; an app itself uses the LcdApp methods, which act on itself and
 * so carry no ordering trap. */

/** Bring an installed program's layer to the front (building it on first use,
 *  exactly as a tile tap would) by its LcdApp::Config::name. No-op if no program
 *  with that name is installed. Must run on the lcd task — call from an lcdRun()
 *  callback or another lcd-task handler. */
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
 *  task; safe to call from any task (it hops on). */
void lcdGoHome(void);

/** Raise the running-app switcher (cards for every app with a live layer) over
 *  whatever is on screen — the same view the half-swipe-up gesture reaches. Runs
 *  on the lcd task; safe to call from any task (it hops on). */
void lcdShowRecents(void);

/** Put the display into / out of standby: backlight off + panel display off (GRAM
 *  retained, so wake is instant) / panel back on with a 300 ms backlight fade-in.
 *  The board owns the standby *policy* — the lcd component's inactivity timeout and
 *  the board's own button only set the ephemeral `sys.standby` key; the board
 *  subscribes to it and calls these (and powers its own input down/up). While
 *  asleep the lcd loop stops rendering so the chip can light-sleep. Lcd task. */
void lcdScreenSleep(void);
void lcdScreenWake(void);

/** Hide the status bar and grow the current program layer to the full screen
 *  height (`on`), or restore both. For an immersive program screen (e.g. a chat
 *  thread). The launcher remembers which layer asked: the bar stays hidden only
 *  while that layer is the one on screen, comes back when it goes Home, and is
 *  re-hidden when the program is re-opened — so callers just toggle it as their
 *  own view changes. Percentage-sized children of the layer reflow to the new
 *  height automatically. Lcd task. */
void lcdProgramFullscreen(bool on);

/** Add a content-agnostic indicator slot to the status bar's right-hand cluster,
 *  just left of the battery/power symbol, and return it (an empty, content-sized
 *  flex row). The caller owns the returned object: it fills it with any LVGL
 *  children (a glyph, signal bars, a coloured dot…), updates them (typically off
 *  a storageSubscribeChanges callback, hopping onto the lcd task), and shows or
 *  hides the slot. The shell only positions the slot — it never interprets the
 *  content — so it stays agnostic to what any consumer chooses to display.
 *  Returns null if the status bar isn't up yet. Lcd task. */
lv_obj_t* lcdStatusbarAddIndicator(void);

/** True while the trackball is in arrow-key mode — an app asked for it with
 *  LcdApp::setScrollwheelArrows(), so the trackball emits arrow keys into the
 *  focus group instead of moving the pointer (an on-device terminal / vim can
 *  then navigate). The board's pointer_read consults this to decide whether to
 *  move the pointer or feed arrows to lcdInputGroup(). Lcd task. */
bool lcdScrollwheelArrowsActive(void);

/** True while a text-entry box's caret is live (blinking / edit mode). Fills the
 *  caret's screen position and whether it sits on the first line. A relative-
 *  pointing HAL (trackball) drives the caret with arrow keys while this holds, and
 *  parks its cursor on the returned point when the user walks out. Independent of
 *  lcdScrollwheelArrowsActive (which is a whole-app latch); a board treats
 *  either as "drive arrows". Any out-param may be null. Lcd task. */
bool lcdCaretActive(int* x, int* y, bool* atTop);

/** Leave text-edit mode: stop the caret blink so a pointing HAL takes its cursor
 *  back. Called by the board on the walk-out gesture (e.g. 3 quick UPs at the top
 *  line); a later click/keystroke in the box re-lights it. Lcd task. */
void lcdCaretRelease(void);

/** Direction for lcdScroll(): the way the content moves to reveal what's
 *  offscreen (DOWN = show the content below, etc.). */
typedef enum { LCD_SCROLL_UP, LCD_SCROLL_DOWN, LCD_SCROLL_LEFT, LCD_SCROLL_RIGHT } lcd_scroll_dir_t;

/** Pan the currently shown program's content (or the launcher when none is up)
 *  `amount` pixels in `dir`. For a touchless, trackball-only board: when the
 *  pointer is driven into a screen edge the board calls this so the trackball
 *  pans the widget under the cursor / scrolls the launcher instead of the
 *  motion being swallowed by the clamp. It locates the relevant scrollable
 *  widget within the active layer and clamps to its range, so it is a no-op when
 *  nothing can scroll that way — UNLESS the shown program registered its own pan
 *  handler (lcdProgramScrollHandler), which then takes the delta instead. Runs on
 *  the lcd task — call from a board pointer_read (already on the lcd task) or an
 *  lcdRun() callback. */
void lcdScroll(lcd_scroll_dir_t dir, int amount);

/** Pan handler a program registers when its content isn't an LVGL scroll
 *  container — e.g. a map canvas it repositions itself. While that program is
 *  shown, lcdScroll() hands it the delta (in pixels) instead of scrolling a
 *  widget. The signs match lv_obj_scroll_by()/lv_indev_get_vect(): +dx reveals
 *  content to the left, -dx to the right, +dy reveals content above, -dy below —
 *  i.e. the same vector a finger drag would produce, so a handler can just add it
 *  to the pan it already accumulates from touch. Runs on the lcd task. */
typedef void (*lcd_scroll_fn_t)(int dx, int dy);

/** Make the current program handle edge-pan itself via `fn` (null clears it),
 *  for content lcdScroll()'s generic widget-scroll can't drive. The binding is
 *  per-program and active only while that program is shown. Lcd task. */
void lcdProgramScrollHandler(lcd_scroll_fn_t fn);

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

/* ---- Screen mirror (remote panel) ----
 * Generic, transport-agnostic display plumbing for a VNC-style mirror of the real
 * panel: hand the pre-swap little-endian RGB565 pixels of each flushed dirty rect
 * to a sink, and inject a remote pointer into the same LVGL context the local
 * panel drives. The lcdmirror straddle wires these to its RLE encoder + WebRTC
 * transport; -lcd knows nothing about RLE / ITS / WebRTC — capturing the
 * framebuffer and injecting a pointer are display plumbing, which -lcd owns. */

/** Sink called from flushCb once per flushed dirty rectangle, BEFORE the in-place
 *  RGB565 byte-swap — so `px` is little-endian (what a browser canvas wants;
 *  tapping after the swap yields mangled colours). `area` is the dirty rect in
 *  display coords; `px` is w*h RGB565 pixels, row-major. Runs on the lcd task
 *  inside the render path: copy what you need and return at once — `px` is LVGL's
 *  draw buffer and is reused after this returns, so do NOT hold a reference. Cheap
 *  memcpy only; run no compression on the lcd task (it shares the SPI bus lock
 *  with LoRa). */
typedef void (*lcd_mirror_sink_t)(const lv_area_t* area, const uint8_t* px);

/** Attach (or detach, with NULL) the mirror sink. One null-check per flush when
 *  detached (negligible). Any task (a single pointer store). */
void lcdMirrorAttach(lcd_mirror_sink_t sink);

/** Pointer state for lcdMirrorInjectPointer. */
typedef enum { LCD_PTR_RELEASED = 0, LCD_PTR_PRESSED = 1 } lcd_ptr_state_t;

/** Inject a remote pointer sample (display coords) into the same LVGL pointer
 *  path the local touch/trackball drives — so a browser mirror clicks the real
 *  UI. Queued and applied on the lcd task. It does not touch the inactivity
 *  timer — the viewer's session holds the panel awake (lcdMirrorKeepAwake), so
 *  per-event pokes would be redundant. Safe from any task. */
void lcdMirrorInjectPointer(int16_t x, int16_t y, lcd_ptr_state_t state);

/** Inject a remote keystroke into the LVGL keypad focus group (lcdInputGroup) —
 *  so a browser mirror can type into the on-device CLI, text fields and menus.
 *  `key` is an LVGL key code: a Unicode codepoint for a printable character, an
 *  LV_KEY_* value for a special key (Enter, Backspace, arrows, …), optionally
 *  OR'd with LCD_KEY_CTRL for a control combo. One call is one keystroke
 *  (press+release are injected). Queued and applied on the lcd task; like the
 *  pointer inject it leaves the inactivity timer alone (the session hold owns
 *  that). Safe from any task. */
void lcdMirrorInjectKey(uint32_t key);

/** Hold the panel awake while a remote viewer is connected. `on` clears the
 *  ephemeral `sys.standby` key — the board's own wake path, so board and panel
 *  agree on the standby state — and suspends the inactivity blank timer, so the
 *  mirror never goes dark under a watching viewer; `off` restores the configured
 *  timeout and re-arms it. Reference to a single viewer session (the webrtc
 *  signalling WS enforces one). Runs on / hops to the lcd task; safe from any
 *  task. */
void lcdMirrorKeepAwake(bool on);

/* ---- Fonts ----
 * The device renders vector faces (TTFs in /fixed/fonts) at any pixel size
 * behind a Kconfig-selectable engine (CONFIG_LCD_FONT_ENGINE); lcdFont() is the
 * one wrapper every consumer uses, returning an lv_font_t* it can hand to
 * lv_obj_set_style_text_font(). Under LCD_FONT_BITMAP the request maps to the
 * nearest compiled bitmap. The bundled bitmap fonts below stay available for
 * fixed-cell / terminal use (and are the sub-10px + BITMAP-mode fallbacks). */

/** A logical face. UI* are proportional, MONO* fixed-width (box-drawing + block
 *  elements for terminal/Micron art), SYMBOLS the LV_SYMBOL_* set. UI_BOLD is a
 *  real SemiBold file; the other bold/italic faces are synthesized (and, under
 *  LCD_FONT_TINY_TTF, degrade to their base face). Must be used on the lcd task. */
enum class LcdFace {
    UI, UI_BOLD, UI_ITALIC,
    MONO, MONO_BOLD, MONO_ITALIC,
    SYMBOLS,
};

/** Resolve (face, px) to a font, created + cached on first use. Never null
 *  (falls back to a bitmap on engine failure or below the vector-size floor).
 *  Its .fallback is chained to the symbol face, so LV_SYMBOL_* renders. Lcd task. */
const lv_font_t* lcdFont(LcdFace face, int px);

/** Drop every cached vector font (a runtime UI-zoom change). Re-resolve through
 *  lcdFont() and re-style any widget holding a freed font before the next
 *  redraw. Lcd task. */
void lcdFontsReset(void);

/** The current UI zoom as a fraction (s.lcd.scale%, clamped 0.5–2.0). Multiply
 *  a base pixel size by this when resolving lcdFont(), so an app's on-device
 *  text scales with the platform zoom like the shell does. An app that names its
 *  own fonts rebuilds at the current scale on its next open. Lcd task. */
float lcdUiScale(void);

/** A base pixel length scaled to the current UI zoom and rounded — `lcdUiScale()`
 *  applied to `px`. Use for any hard-coded pixel dimension (margins, paddings,
 *  gaps, font sizes) so on-device layout tracks the platform zoom. Lcd task. */
int lcdPx(int px);

/* ---- Runtime SVG icons ----
 * Straddles ship icon *sources* to /fixed/icons/<base>.svg (see the
 * spangap_lcd_icons() build helper); the device rasterizes them on demand at an
 * exact pixel size and RAM-caches the result. The rasters are monochrome — tint
 * at use with the image recolor style. All three run on the lcd task. */

/** Ask the loader to rasterize /fixed/icons/<base>.svg at `px` (no-op if already
 *  cached). Rasterization is off-task, so the descriptor isn't ready on return;
 *  set it from your own periodic refresh once lcdIconReady()/lcdIconDsc() report
 *  it landed. */
void lcdIconRequest(const char* base, int px);

/** True once (base, px) is rasterized and cached. */
bool lcdIconReady(const char* base, int px);

/** The cached descriptor for (base, px), or nullptr until it lands. The pointer
 *  is stable for the cache's lifetime — hand it to lv_image_set_src(). A UI-zoom
 *  icon-cache reset invalidates it, so re-source on your next rebuild. */
const lv_image_dsc_t* lcdIconDsc(const char* base, int px);

/* Bundled LVGL bitmap fonts a program may set directly on its own widgets with
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

/* ---- Settings ----
 *
 * ONE TREE, not a set of panes. Every node holds rows AND children: entering it
 * renders its rows first, then a chevron row per child. No node is owned — a
 * contribution names a path, every intermediate node on the way is conjured,
 * and several contributors at the same path simply concatenate their row
 * blocks. The path is the node's stable id on both this surface and the web's.
 */

/** Contribute a row block at a path. `segs` names the path from the Settings
 *  root, one lcd_seg_t per level, each carrying the id (the stable merge key)
 *  and the naming this contributor proposes: `label` (the long name, shown in
 *  the parent's navigation row), `shortName` (the pane header) and `order`. The
 *  LAST contributor to name a node wins. `fn` is called on the lcd task with
 *  the node's content widget (an empty, scrollable flex column — build the rows
 *  into it with the lcdSetting* helpers) and may be null for a contribution
 *  that only names nodes; a node left with no rows and no rendering descendant
 *  gets no navigation row, so naming a menu does not by itself show one.
 *
 *  Siblings sort by the one global rule: nodes carrying an order first,
 *  ascending; the rest after them in contribution order. Row blocks at one node
 *  concatenate in contribution order too. Call at init, before Settings is
 *  first opened — this populates a registry, so it works even before lcdInit()
 *  and from any init task. */
void lcdSettingsContribute(const lcd_seg_t* segs, int nsegs, lcd_fn_t fn);

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
/** Slider bound to an int key, clamped to [min,max]. `minKey`/`maxKey` name
 *  storage keys the device publishes its real limits under; where one exists it
 *  replaces the compiled bound when the row is built, so a control can be sized
 *  to hardware the build could not know about. */
lv_obj_t* lcdSettingSlider  (lv_obj_t* parent, const char* label, const char* key, int min, int max,
                             const char* minKey = nullptr, const char* maxKey = nullptr);
/** Text field bound to a string key; tap opens an on-screen keyboard. When
 *  `secret`, the value is masked. */
lv_obj_t* lcdSettingText    (lv_obj_t* parent, const char* label, const char* key, bool secret = false);
/** Dropdown bound to a string key; `optionsCsv` is a comma-separated list. */
lv_obj_t* lcdSettingDropdown(lv_obj_t* parent, const char* label, const char* key, const char* optionsCsv);
/** Read-only row showing whatever string the key holds — the firmware publishes
 *  the finished text, so this never formats or composes. Event-driven. */
lv_obj_t* lcdSettingValue   (lv_obj_t* parent, const char* label, const char* key);
/** Action button; `onClick` runs on the lcd task (arg is the row). */
lv_obj_t* lcdSettingButton  (lv_obj_t* parent, const char* label, lcd_fn_t onClick);

/** Show `row` only while `key` is truthy (non-empty, non-zero). Subscribes for
 *  the row's lifetime and unsubscribes on its delete; returns `row`, so it wraps
 *  any helper above: lcdSettingWhenKey(lcdSettingSwitch(p, …), "wifi.up"). */
lv_obj_t* lcdSettingWhenKey (lv_obj_t* row, const char* key);

/* ---- Info groups ----
 * A run of read-only values as one compact block: a shared label column sized to
 * the widest label but never wider than the third an ordinary row gives its
 * label, and no gap between the lines. A column of short labels therefore reads
 * narrow instead of leaving a third of the pane empty.
 *
 * Read-only values only. The narrow shared column is a promise a control cannot
 * keep — a switch or a slider needs its own room — so anything interactive is an
 * ordinary row above or below the group.
 *
 *   lv_obj_t* g = lcdSettingInfo(p);
 *   lcdSettingInfoValue(g, "Status", "wifi.sta.state_text");
 *   lcdSettingWhenKey(lcdSettingInfoValue(g, "IP", "wifi.sta.ip"), "wifi.sta.up");
 *   lcdSettingInfoFit(g);
 */

/** An empty info group. Add lines with lcdSettingInfoValue, then close it with
 *  lcdSettingInfoFit — the column is only knowable once every label is in. */
lv_obj_t* lcdSettingInfo     (lv_obj_t* parent);
/** One line: label, and whatever string the key holds. Returns the line, so
 *  lcdSettingWhenKey wraps it exactly as it wraps an ordinary row. */
lv_obj_t* lcdSettingInfoValue(lv_obj_t* group, const char* label, const char* key);
/** Size the shared label column and close the group. Measures every label,
 *  hidden ones included, so a `when_key` line appearing never shifts the column. */
void      lcdSettingInfoFit  (lv_obj_t* group);

/** Button running a descriptor action — a key write, a confirmation dialog, or
 *  a form over a command sentinel. `color` is a palette name or "rrggbb"
 *  (lcd_settings_desc.h); null leaves the button its default colour. */
lv_obj_t* lcdSettingAction  (lv_obj_t* parent, const char* label, const lcd_action_t* act,
                             const char* color = nullptr);

/** Several content-sized action buttons sharing one row, placed left, centre or
 *  right. A lone `lcdSettingAction` still spans the pane; this is the row for
 *  buttons that belong beside each other. */
lv_obj_t* lcdSettingActionRow(lv_obj_t* parent, lcd_align_t align,
                              const lcd_btn_t* btns, int nbtns);

/** A collection: the array at `desc->key` as titled item rows with an editor,
 *  add forms, removal and optional reordering, all through `desc->cmd`
 *  sentinels. Rebuilds itself on array changes; tears its subscriptions down
 *  with the pane. */
lv_obj_t* lcdSettingCollection(lv_obj_t* parent, const lcd_collection_t* desc);

/** Run an action from anywhere (a hand-written button, a launcher tile).
 *  `itemPrefix` scopes `{field}` substitution to one collection item (the
 *  storage prefix of that item, e.g. "s.tcp.peers.2"), and `idValue` is what
 *  `{id}` resolves to; both null outside a collection. */
void lcdSettingRunAction(const lcd_action_t* act, const char* itemPrefix = nullptr,
                         const char* idValue = nullptr);

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
