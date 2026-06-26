/**
 * nav.cpp — navigation producers that feed the single shellNavigate() consumer.
 *
 * M3 provides the ESC -> BACK producer (the gesture-bar HOME/RECENTS producer
 * lives with the chrome in manager.cpp; the board centre-button already calls
 * lcdGoHome -> shellNavigate(HOME)). The keyboard delivers LV_KEY_ESC to whatever
 * is focused in lcdInputGroup(); LVGL has no group-wide key hook, so we track the
 * focused object via the group's focus callback and keep a key handler on it.
 *
 * ESC is passed THROUGH (not consumed) when the foreground app owns raw keys —
 * a terminal/arrow-mode app (CLI, vim over ssh) or a focused text field — so the
 * gesture only means "Back" where it would otherwise do nothing useful. No board
 * edit and no new hardware (plan §5.4).
 */
#include "shell_internal.h"
#include "lcd_internal.h"   /* lcdInputGroup */

namespace {

lv_obj_t* s_hookObj = nullptr;

bool focusedWantsEsc() {
    LcdApp* fg = shellForeground();
    if (fg && fg->_arrows()) return true;                 /* terminal / arrow-mode app */
    if (s_hookObj && lv_obj_check_type(s_hookObj, &lv_textarea_class)) return true; /* editing text */
    return false;
}

void keyHook(lv_event_t* e) {
    if (lv_event_get_key(e) != LV_KEY_ESC) return;
    if (focusedWantsEsc()) return;                         /* let the widget have ESC */
    if (!shellForeground() && !shellRecentsVisible()) return;  /* nothing to back out of */
    shellNavigate(NavIntent::BACK);
}

/* Move the key handler to whatever is now focused. */
void onFocusChange(lv_group_t* g) {
    if (s_hookObj && lv_obj_is_valid(s_hookObj))
        lv_obj_remove_event_cb(s_hookObj, keyHook);
    s_hookObj = lv_group_get_focused(g);
    if (s_hookObj) lv_obj_add_event_cb(s_hookObj, keyHook, LV_EVENT_KEY, nullptr);
}

}  // namespace

void shellNavInstall(void) {
    lv_group_t* g = lcdInputGroup();
    if (!g) return;
    lv_group_set_focus_cb(g, onFocusChange);
    onFocusChange(g);   /* attach to whatever is focused at boot (a launcher tile) */
}
