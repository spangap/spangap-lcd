/**
 * lcd_panel.h — what a board tells spangap-lcd about its panel that Kconfig
 * cannot carry.
 *
 * A MIPI-DSI panel's controller is configured over the link itself, in command
 * mode, before the video stream starts — and the configuration is a vendor
 * table for the specific glass (gamma, power, gate timing), not something any
 * generic description produces. spangap-lcd owns the link, so it sends the
 * table; the board owns the glass, so it supplies it, from a Service onStart
 * (start band, before spangap-lcd brings the panel up in the init band).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One controller command: `cmd` with `len` parameter bytes from `data`, then a
 *  wait of `delay_ms` before the next. `data` may be NULL when `len` is 0. */
typedef struct {
    uint8_t        cmd;
    const uint8_t* data;
    uint8_t        len;
    uint16_t       delay_ms;
} lcd_init_cmd_t;

/** Register the panel controller's init sequence, sent in order over the DSI
 *  command channel after the panel's reset and before its video starts. The
 *  table must outlive the process (a static). Call before the panel comes up —
 *  from a Service onStart. Only the DSI transport reads it. */
void lcdPanelSetInitSequence(const lcd_init_cmd_t* cmds, size_t count);

#ifdef __cplusplus
}
#endif
