# Settings — internals

Maintainer reference for `lcd_settings.cpp` — the built-in Settings app, its
node registry, the page-stack nav, the `lcdSetting*` helpers and the two-way
storage binding — and for `lcd_settings_desc.cpp`, the generic runtime behind
the descriptors (actions, dialogs, forms, collections). The
[operator guide](settings.md) is the author view. Everything runs on the lcd
task.

## 1. What settings adds

- **`lcdSettingsContribute(segs, nsegs, fn)`** — merges a contribution into an
  in-RAM tree of `Node`s, conjuring whatever the path is missing and appending
  `fn` to the node's builder list. A `Node` holds BOTH builders and children;
  there is no leaf flag, because there is no leaf/container distinction. Naming
  is last-setter-wins per field, and a node nobody names keeps its title-cased
  id (`named`/`shortNamed` only record that somebody did, so the short name can
  keep tracking the label). A plain tree, so registration works before
  `lcdInit()` and from any task.
- **`nodeRenders(n)`** — a node shows only if it has builders or a descendant
  that does. `pushNode` filters the children through it, so a menu declared for
  its name and order alone (spangap-core's `apps` / `system`, reticulous's
  `reticulum`) contributes no dead-end chevron row.
- **The `lcdSetting*` helpers** — storage-bound row builders.
- **`SettingsApp`** — a thin `LcdApp` host (gear tile, `Config::name = "Settings"`,
  `iconBasename = "gear"`) installed by `lcdSettingsInit()`
  from `shellInit`. Its `onCreate` calls `settingsOpen(root)`; `onClose` clears
  the page-stack pointers. The registry, the builders, two-way binding, scroll
  pills, and every straddle's pane hook are unchanged by the app wrapper.
- **The Display section of System** — described in `spangap-lcd/straddle.yaml`
  like any other straddle's settings, not hand-written here: a step dropdown
  over `s.lcd.scale`. It only writes the key; the restart that applies it is the
  `lcd.cpp` subscription (shell-internals §10), so a browser/CLI write behaves
  identically.

## 2. The page-stack nav

Each menu level and each item pane is its own opaque, full-size **page** stacked
in a host container below a shared header. Descending pushes a new page **on
top** — the parent stays alive, untouched, beneath it; Back deletes the top page,
revealing the parent exactly as it was (scroll position included). The header
(back chevron + breadcrumb title, e.g. `Settings/Net/Wifi`) lives **outside** the
pages, so Back never deletes the widget whose event it's handling, and descending
never deletes the row being clicked.

The generator contributes **once per node**, not once per block, because that is
the only place two straddles' rows can be interleaved — a builder is opaque by
the time it reaches here. Sections merging under one heading is that interleave;
see the build-system README. A hand-written `lcdSettingsContribute` runs from
lcd init, ahead of `spangapSettingsGenRegister()`, so its builder renders before
the generated one whatever order the yaml states — it cannot join a generated
section, and a row that has to sit inside one belongs in a `settings:` block.

- `pushNode(node)` makes a page, pushes it, runs every contributed builder onto
  it in order, then appends a navigation row (label + chevron, carrying the
  `Node*`) per child. Children sort by `nodeLess`: nodes carrying an order
  first ascending, the rest after them in contribution order. That is the one
  rule the web store and the generator also use — and contribution order is
  meaningful because the generator emits pre-sorted, in init order. The page is
  on the stack BEFORE the builders run, so a builder that opens a modal sees a
  coherent stack.
- `popPage()` deletes the top page; at the root it exits via `lcdGoHomeInternal()`.
- **Scroll-overflow pills** — small `↑`/`↓` chips at the host's right edge,
  shown from the top page's scroll bounds, re-floated above each freshly pushed
  page. All pages scroll (a menu can outgrow the viewport too); LVGL's
  scroll-vs-tap threshold keeps row clicks working.

The breadcrumb is built from the nodes' SHORT names — this is a 30px strip on a
phone-sized screen, which is the whole reason a node may carry a short name
distinct from its long label. It uses `lcdFont(LcdFace::UI_BOLD, 16)`;
navigation rows use `lcdFont(LcdFace::UI, 16 × lcdUiScale())`.

## 3. Two-way storage binding

Every storage-bound control registers a `Bind { key, widget, kind, secret }` in
the file-static `s_binds`. The control's `LV_EVENT_VALUE_CHANGED` writes the key
(`storageSet`); separately the key is **subscribed** (`storageSubscribeChanges` →
`bindDispatch`) so an external write flows back into the widget via `bindApply`,
switched on `BindKind` (`BK_SWITCH`/`BK_SLIDER`/`BK_DROPDOWN`/`BK_TEXTLBL`/
`BK_TEXTAREA`/`BK_VALUE`/`BK_WHENKEY`). Storage callbacks dispatch on the lcd
task, so
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
better — dropdown / value / slider group). `makeRow(parent, compact)` gives ¾ of
`SETTINGS_ROW_H` for the rows inside a modal.

**Vertical economy** is deliberate and lives in three places, because a pane of
stacked rows on a 240 px panel runs out of screen long before it runs out of
width. Every button and input field goes through `halfPadVer`, which halves
whatever vertical padding the theme gave it (halves rather than imposes, so the
widget keeps the theme's proportions); the page's `pad_row` is 4; and the page
carries `SETTINGS_ROW_H / 2` of bottom padding, so the end of a scrolled pane
reads as an end rather than as content cut off at the edge of the glass.

- **Switch** — a compact `lv_switch` (36×18) with a high-contrast off state.
- **Slider** — a `lv_slider` that grows to fill the control column, with a live
  numeric readout right-aligned after it; both bind the key (`BK_SLIDER` +
  `BK_VALUE`) so an external write refreshes the number too. The readout is mono
  in a fixed six-digit column (`numColWidth`) shared by every slider in the
  build, NOT sized per range — sized per range, each row's slider started at a
  different x. LVGL centres the knob on the end of the indicator, so half of it
  hangs outside the slider's box at either bound; the room for that
  (`height/2 + knob pad + LV_DPX(3)` for the pressed grow) is the GROUP's
  `pad_left` and `pad_column`, never a margin on the slider — see the
  grow-item-margin gotcha in the straddle README.
- **Text** — two paths on `lcdHasKeyboard()`: inline `lv_textarea` (joined to the
  focus group, committing on `LV_EVENT_READY`/`DEFOCUSED`) vs a full-screen
  `lv_keyboard` overlay over a value label. The overlay's `TextRef` is
  `gp_alloc`'d and freed on the row's `LV_EVENT_DELETE`.
- **Dropdown** — CSV → newline options; `dropdownSelect` matches the stored value
  by option text.
- **Value** — a label bound `BK_VALUE`, event-driven (the em-dash `—` for empty).
  Long-value rendering is the marquee/wrap split below.
- **Button** — `onClick` is an `lcd_fn_t` invoked with the row as `arg`.
- **Info groups** — `lcdSettingInfo` / `lcdSettingInfoValue` / `lcdSettingInfoFit`.
  Ordinary storage-bound rows on the same `bindAttach` table, in a zero-row-gap
  flex column, with one difference: their label column is shared and sized by
  the group rather than fixed at `lv_pct(33)`. LVGL has no cross-sibling
  max-content, so `Fit` is where the width comes from — it runs
  `lv_obj_update_layout`, reads each label's natural width, caps the widest at a
  third of the group's content width, and writes it back to all of them. That is
  the whole reason the group is three calls: the caller is what knows the run
  has ended. `Fit` walks the group by child position (child 0 of each line is
  the label), which is why `InfoValue` creates the label first.
- **`lcdSettingActionRow`** — a `makeRow` switched to `ROW_WRAP` with a
  content height, holding `LV_SIZE_CONTENT` buttons; `align` is the flex main
  alignment. Wrapping is deliberate: a fixed-height non-wrapping row would clip
  a pair of long labels off the edge of the display instead of stacking them.
  A per-button `when_key` is `lcdSettingWhenKey` on the button object.
- **`buttonColor`** — one palette for every button, resolved through the same
  `pillColor` table a status pill uses, so a red button is the red a red pill
  is. `lcd_dlg_btn_t`, `lcd_item_action_t` and `lcd_btn_t` all carry `color` as
  a `const char*`; null is the button's own colour.
- **`lcdSettingWhenKey(row, key)`** — rides the same binding table as a
  `BK_WHENKEY` bind, so it inherits subscribe-once, unsubscribe-with-the-last-user
  and tear-down-on-delete for free, and needs no machinery of its own. It
  toggles `LV_OBJ_FLAG_HIDDEN` on truthiness only; the firmware publishes gate
  keys truthy/empty so an equality test never has to exist.

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
- **A modal must be dismissed asynchronously** — a modal's own button is a
  descendant of the overlay it closes, so `lcd_settings_desc.cpp` dismisses via
  `lv_obj_delete_async`. For the same reason a form's context is freed by the
  overlay's `LV_EVENT_DELETE`, not by `formClose()`: the field widgets' callbacks
  point into it and it has to outlive the deferred delete.
- **Deleting the focused widget moves the ring, and LVGL's default choice is
  wrong here.** Whatever is deleted, the ring has to land somewhere: LVGL walks
  the group — one global list in insertion order, never cleared — from the
  object being removed. Its stock policy is the PREVIOUS member, which walks
  backwards out of a teardown and, from the first member, wraps onto the last
  widget ever added: the bottom row of the pane, dragged into view by the
  scroll-on-focus every object carries, blinking a caret if that row is a text
  field. `lcd_lvgl.cpp` sets `LV_GROUP_REFOCUS_POLICY_NEXT` (forwards is the
  direction a teardown goes — off the end of a deleted list and onto the
  controls after it), and every overlay states its own answer with
  `lcdSettingsRefocusOnClose`: the widget it took focus from, or the top of the
  visible page if that widget did not survive. It fires on the overlay's
  `LV_EVENT_DELETE`, which LVGL sends BEFORE deleting children, so there is
  nothing focused left to walk from — and only when the ring is still inside
  that overlay, so a page closing to open a dialog does not pull focus out from
  under it.
- **Never rebuild a collection while walking `s_pills`** — a rebuild deletes
  widgets, whose delete callbacks mutate that vector. `collStorageCb` therefore
  notes what changed in one pass and rebuilds in a second.

## 7. The descriptor runtime (`lcd_settings_desc.cpp`)

Simple rows stay CALLS (that is what a hand-written pane writes, and the two
should be indistinguishable); anything richer is DATA. `lcd_settings_desc.h`
declares the structs, this file is the single place that knows what "confirm
then write a key", "collect fields and submit them to a sentinel" or "list an
array with an editor" mean. Generating data instead of logic keeps the generated
dispatch file small and puts the behaviour somewhere reviewable — the same
argument that chose a runtime-interpreted descriptor over generated Vue
components on the web side.

- **Substitution** is `{field}` replacement and nothing else. `subst()` resolves
  against an `ItemScope` (one collection item's storage prefix, plus what `{id}`
  means there); a form resolves against its local field buffer instead. A form's
  `when_key` tells the two apart by whether the template contains a brace: a
  braced gate names a sibling field, a bare one names storage.
- **Subscriptions are refcounted PER SCOPE** (`subAdd`/`subDrop`), with one
  dispatcher (`descDispatch`) serving pills, collection rebuilds and the open
  form alike. Per scope, not per (scope, callback): with per-pair counts a
  form dropping its watch on a collection's answer keys would take them out
  from under the collection. One callback per scope, however many users.
  A status-key template is subscribed at its literal prefix (everything before
  the first brace), since subscriptions are prefix-matched.
- **Both tables drop their watch with `storageUnsubscribeCb`, never
  `storageUnsubscribe`.** The lcd task is shared: the shell watches keys of its
  own on it (backlight, inactivity timeout, UI zoom, a board's panel keys), and
  `storageUnsubscribe(scope)` drops *every* subscription the calling task holds
  on that scope — so a settings page that unsubscribed by scope would silently
  disconnect the module that owns the key, and the setting would go on writing
  it with nothing left to apply it. Unsubscribing the exact callback keeps the
  descriptor runtime, the plain-row binding table in `lcd_settings.cpp`, and
  the key's owner independent of one another.
- **Forms** hold their values locally and reach the device only on submit, which
  is what makes submit-and-error possible in place of per-keystroke validation.
  One form is open at a time. The handler answers on the sentinel family's
  error/ack pair (a collection hands its forms the shared `<cmd>.error` /
  `<cmd>.done`; a bare form derives `<form-cmd>.error` / `.done`): the error key
  going non-empty shows the reason and keeps the form open, the ack key moving
  after a submit closes it — an edit that changes nothing still acks. Submit
  clears the error key first, in order, so an identical rejection is still a
  change past the storage actor's write-dedup. No timeouts, no read-back. An
  item editor additionally carries `_id` — the identity it commits against — so
  editing the id field itself is an ordinary edit and the owning task still
  knows which item to apply it to.
- **Modal lifetime.** Modals live on `lv_layer_top`, outside the app's widget
  tree, so nothing tears them down implicitly. Every overlay is tracked
  (`s_modals`); `lcdSettingsDescReset()` (called from the Settings app's
  `onClose`) force-closes the open form, the on-screen-keyboard editor and any
  remaining dialogs. A form opened by a collection carries an `owner` pointer,
  and the collection's teardown (`collDelete`) force-closes it — the item
  editor's descriptor lives inside the collection context and must not outlive
  it. Closing from a modal's own button stays deferred (`dismiss` →
  `lv_obj_delete_async`, untracked at dismiss time so a later reset cannot
  double-delete); closing from outside its events is synchronous
  (`formForceClose`).
- **Collections** never write the array. `<cmd>.add` / `.remove` / `.set` /
  `.order` are composed from the one `cmd` name, and a reorder sends the whole
  id order comma-joined for the firmware to apply as a preference permutation.
  The pane's teardown (`collDelete`) also clears a `candidates` refresh target
  key, which is the entire "stop scanning on leave" contract.
- **An item's buttons are on its detail page, not its row.** `collRebuild` gives
  a row the reorder arrows and nothing else; if `hasDetailPage()` (any `edit`
  rows, any `actions`, or `remove`) the whole row is clickable and opens the
  editor form with `itemIdx` set, and `buildItemButtons` appends Delete and the
  per-item actions to that form's button row. The editor's `title` is
  deliberately null — the collection's name says nothing about *which* item, and
  a `section:` row templated over the item does. `formLookup` falls back to
  `fieldOf(sc, name)` when the form has no field of that name, which is what
  makes `section: "{ssid}"` resolve on a page that does not offer the SSID as a
  row. The collection's own buttons (a `candidates` scan, then each `add:`) share
  one right-gathered `buttonBar` under the list.
- **The scan popup.** `candidates` render into `CollCtx::candBox`, which exists
  only while the popup does: `onRefreshButton` builds the modal, points `candBox`
  at its scroll body, runs `refresh`, and `candRebuild` fills it off the same
  `descDispatch` subscription the pane holds (so a scan already running fills it
  at once). The card is built by `makeModal`'s `closeCb` form, which puts Close
  in the title row's right corner instead of at the foot. `candPopupClose`
  clears the refresh key and dismisses. Two teardown
  hazards, both handled: the popup lives on `lv_layer_top` so `collDelete` has to
  close it explicitly, and `dismiss` is async — so the overlay's delete callback
  finds its collection by scanning `s_colls` for the overlay rather than carrying
  a `CollCtx*`, which a torn-down collection would leave dangling. Each of those closes the form
  *before* it runs (`onDetailButton` copies the scope out first, since the close
  frees its context): an action can invalidate the page it was pressed on, and a
  removal takes the item out from under it, so a confirmation must not stack on
  top of it. An action's `when_key` is `subst`-ed against the item into a real
  storage key and handed to `lcdSettingWhenKey`, which copies it — the binding
  table stores `std::string`, so unlike the pane helpers this one takes a
  temporary safely.
- **Modal geometry.** Everything below the title lives in one scroll container
  capped at ⅔ of the screen: fields, the rejection label, then the button row.
  The buttons are content-sized in a right-gathered `ROW_WRAP`
  (`modalButtonRow` / `modalButton`) rather than the full-width stack they were,
  and the container carries a smaller font while the field rows are built
  `compact` — three quarters of `SETTINGS_ROW_H`. A fixed footer on a 240 px
  panel is a third of the dialog.
