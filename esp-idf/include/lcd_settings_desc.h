/**
 * lcd_settings_desc.h — the settings DESCRIPTOR types.
 *
 * Everything a settings pane can do that is more than a labelled row — actions,
 * dialogs, forms, collections — is described by one of these structs and run by
 * a generic function in lcd_settings_desc.cpp. The build (spangap-inside) emits
 * them as static literals from each straddle's `settings:` block; a hand-written
 * pane may equally declare one and call the same runtime.
 *
 * Data, not logic, deliberately: a generated file full of structs stays small
 * and reviewable, and the behaviour lives in one place instead of being
 * re-emitted per pane. Every string here is a literal or otherwise outlives the
 * pane — the settings helpers store keys BY POINTER (panes are built and
 * destroyed as you navigate), so a descriptor must never point at a temporary.
 *
 * Two conventions the descriptors assume, both firmware-side:
 *   - The firmware publishes FINISHED STRINGS. A status pill, a subtitle, a
 *     value row: whatever the key holds is shown verbatim. Nothing here formats,
 *     composes or compares — a `when_key` gate is a truthiness test, never an
 *     equality one.
 *   - The firmware VALIDATES IN SENTINEL HANDLERS. A form submits JSON to its
 *     `cmd` key; the owning task validates and answers on the sentinel family's
 *     shared keys — a rejection is a reason on `<cmd>.error` (the form shows it
 *     and stays open), an accepted mutation bumps `<cmd>.done` (the form
 *     closes). There is no validation on this side.
 */
#ifndef SPANGAP_LCD_SETTINGS_DESC_H
#define SPANGAP_LCD_SETTINGS_DESC_H

#include "lvgl.h"

struct lcd_action_t;
struct lcd_form_t;
struct lcd_collection_t;

/* ---- rows ---- */

typedef enum {
    LCD_ROW_SECTION, LCD_ROW_CAPTION, LCD_ROW_SWITCH, LCD_ROW_SLIDER,
    LCD_ROW_TEXT, LCD_ROW_DROPDOWN, LCD_ROW_VALUE, LCD_ROW_BUTTON,
    LCD_ROW_LIST,
} lcd_row_kind_t;

/** One row, as data. A pane's own rows are emitted as lcdSetting* CALLS (that
 *  is what a hand-written pane writes, and the two should be indistinguishable);
 *  this struct describes the rows that live inside a form or an item editor,
 *  where the runtime builds them against a local field buffer rather than
 *  against storage. `key` binds storage, `field` binds that buffer — exactly
 *  one of the two is set.
 *
 *  Every member here (and in the structs below) carries a default. Descriptors
 *  are written by a generator that names only the fields a row actually uses,
 *  and a default per member is what makes that legal designated-initializer C++
 *  rather than a wall of nullptrs — and what keeps the compiler quiet about the
 *  rest. */
typedef struct lcd_row_t {
    lcd_row_kind_t kind        = LCD_ROW_SECTION;
    const char* label          = nullptr;   /* also the text of a section / caption */
    const char* key            = nullptr;   /* storage-bound row */
    const char* field          = nullptr;   /* form / item-editor field name */
    const char* when_key       = nullptr;   /* only while truthy; "{field}" in a form */
    int         min            = 0;
    int         max            = 0;
    const char* min_key        = nullptr;   /* device-published bounds, overriding these */
    const char* max_key        = nullptr;
    bool        secret         = false;
    bool        copyable       = false;     /* value rows; the web's copy affordance */
    const char* options        = nullptr;   /* dropdown values, newline-separated */
    const char* opt_labels     = nullptr;   /* dropdown labels, 1:1 with options */
    const char* placeholder    = nullptr;
    const char* dflt           = nullptr;   /* form prefill; may template. Never seeded */
    const struct lcd_action_t* act      = nullptr;   /* button */
    const struct lcd_collection_t* coll = nullptr;   /* list */
} lcd_row_t;

/* ---- actions ---- */

typedef enum { LCD_ACT_SET, LCD_ACT_DIALOG, LCD_ACT_FORM } lcd_action_kind_t;

typedef struct lcd_dlg_btn_t {
    const char* label  = nullptr;
    bool        danger = false;
    const struct lcd_action_t* act = nullptr;   /* null: a bare cancel */
} lcd_dlg_btn_t;

/** A confirmation or a choice. No input fields, ever — that is what a form is
 *  for. Every button closes the dialog; buttons nest actions, so a choice tree
 *  is dialogs of buttons of sets. */
typedef struct lcd_dialog_t {
    const char*          text     = nullptr;
    const lcd_dlg_btn_t* buttons  = nullptr;
    int                  nbuttons = 0;
} lcd_dialog_t;

/** The one dialog that carries inputs, because it fronts a sentinel. Submitting
 *  serializes the collected fields as a JSON object to `cmd`. */
typedef struct lcd_form_t {
    const lcd_row_t* fields  = nullptr;
    int              nfields = 0;
    const char*      cmd     = nullptr;
    const char*      submit  = nullptr;   /* submit-button label */
    const char*      title   = nullptr;
} lcd_form_t;

typedef struct lcd_action_t {
    lcd_action_kind_t kind = LCD_ACT_SET;
    const char*  key     = nullptr;   /* LCD_ACT_SET */
    const char*  value   = nullptr;
    bool         edge    = false;     /* write 0 first, forcing a change past the dedup */
    bool         reboots = false;     /* show a modal notice; the device is going away */
    const lcd_dialog_t* dialog = nullptr;   /* LCD_ACT_DIALOG */
    const lcd_form_t*   form   = nullptr;   /* LCD_ACT_FORM */
} lcd_action_t;

/* ---- collections ---- */

typedef struct lcd_add_t {
    const char*        label = nullptr;
    const lcd_form_t*  form  = nullptr;
} lcd_add_t;

typedef struct lcd_item_action_t {
    const char*         label  = nullptr;
    bool                danger = false;
    const lcd_action_t* act    = nullptr;
} lcd_item_action_t;

/** Scan-and-adopt. The owning task publishes an ephemeral array; picking one of
 *  its rows opens the collection's first add form, prefilled. The runtime
 *  clears the refresh target key when the pane goes away, so nothing has to
 *  carry a "stop scanning on leave" timer. */
typedef struct lcd_candidates_t {
    const char*         key           = nullptr;
    const char*         item          = nullptr;  /* "{field}" over what the task publishes */
    const char*         subtitle      = nullptr;
    const char*         refresh_label = nullptr;
    const lcd_action_t* refresh       = nullptr;
    const char*         map           = nullptr;  /* field renames, "from:to,from:to" */
} lcd_candidates_t;

/** An array-of-objects in storage, edited entirely through sentinels: the UI
 *  never writes the array, the owning task is its only writer. */
typedef struct lcd_collection_t {
    const char* label     = nullptr;
    const char* key       = nullptr;   /* the array */
    const char* id        = nullptr;   /* the field identifying an item */
    const char* item      = nullptr;   /* row title template */
    const char* subtitle  = nullptr;
    const char* status    = nullptr;   /* ephemeral key template, packed "text|color" */
    const char* empty     = nullptr;
    bool        orderable = false;
    const char* cmd       = nullptr;   /* <cmd>.add/.remove/.set/.order, answered on <cmd>.error/.done */
    const lcd_add_t* adds = nullptr;
    int         nadds     = 0;
    bool        has_remove     = false;
    const char* remove_confirm = nullptr;   /* empty: remove without confirming */
    const lcd_item_action_t* actions = nullptr;
    int         nactions  = 0;
    const lcd_row_t* edit = nullptr;   /* the item editor's rows */
    int         nedit     = 0;
    const lcd_candidates_t* candidates = nullptr;
} lcd_collection_t;

/* ---- one node's naming, as contributed ---- */

typedef struct lcd_seg_t {
    const char* id        = nullptr;
    const char* label     = nullptr;
    const char* shortName = nullptr;   /* the header text; defaults to the label */
    int         order     = 0;
    bool        has_order = false;
} lcd_seg_t;

#endif
