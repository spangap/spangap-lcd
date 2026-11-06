/**
 * lcd_keys.h — held-key state for named keys.
 *
 * A typed key is an event; some keys are instead held, and what matters is how
 * long — a push-to-talk key keys a transmitter for exactly as long as it is
 * down. A keyboard that can see key-up reports those keys here by NAME, and
 * tasks that act on them subscribe and receive each down/up edge as an ITS aux
 * message. The board owns the keyboard and the subscribers own the meaning;
 * this layer only names the keys and fans the edges out. The subscriber list
 * lives on the lcd task.
 *
 *   board     lcdKeyDeclare(LCD_KEY_PTT)            once, at keyboard bring-up
 *   board     lcdKeyReport(LCD_KEY_PTT, down)       on every edge, any task
 *                 ▼  (wakes the lcd task, which fans out)
 *   consumer  itsOnAux(port, cb); lcdKeySubscribe(port)
 *   consumer  cb(sender, lcd_key_edge_t*)          one per edge
 *
 * Claiming. A named key may also be an ordinary typing key (Space is). Its
 * edges are delivered to subscribers only while some consumer has CLAIMED it
 * with lcdKeyClaim(), and while it is claimed the board must not also type it:
 * lcdKeyReport() returns true for a claimed key, telling the board to swallow
 * the press. An unclaimed key types as usual and no edge is sent. A press
 * belongs to the claim it began under: one that began unclaimed stays the
 * board's for as long as it is held, even if a claim arrives meanwhile. A claim
 * is a count, so independent claimants nest.
 *
 * A board with no keyboard that can see key-up declares nothing, and
 * lcdKeyDeclared() tells a consumer to offer an on-screen hold button instead.
 */
#ifndef SPANGAP_LCD_KEYS_H
#define SPANGAP_LCD_KEYS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The named keys. */
typedef enum {
    LCD_KEY_PTT   = 0,   /* a dedicated talk key (the T-Deck's Mic key)        */
    LCD_KEY_SPACE = 1,   /* the space bar, held                                 */
    LCD_KEY_COUNT
} lcd_named_key_t;

/** The aux payload a subscriber receives, one per edge. */
typedef struct {
    uint8_t key;    /* lcd_named_key_t */
    uint8_t down;   /* 1 = pressed, 0 = released */
} lcd_key_edge_t;

/** Board: this keyboard reports `key`. Call once per key at bring-up. */
void lcdKeyDeclare(uint8_t key);

/** Board: this keyboard no longer reports `key` (it fell back to a mode that
 *  cannot see the key's up). Report a held press up before undeclaring it. */
void lcdKeyUndeclare(uint8_t key);

/** Whether the board reports `key`. */
bool lcdKeyDeclared(uint8_t key);

/** Whether some consumer has `key` claimed now. A board that holds a key down
 *  on the user's behalf (a latching toggle) releases it when this goes false. */
bool lcdKeyClaimed(uint8_t key);

/** Board: call `cb(key, claimed)` each time `key` becomes claimed or stops
 *  being claimed, on the claiming task; `cb` must not wait. A board whose scan
 *  depends on the claim (one that keeps watching a claimed key while the screen
 *  is off) uses it to wake that scan. One watcher; NULL removes it. */
void lcdKeyOnClaimChange(void (*cb)(uint8_t key, bool claimed));

/** Board: `key` went down or up. Callable from any task, never waits. On a
 *  down, returns true when the key is claimed — the board then swallows the
 *  press instead of typing it. On an up, returns whether that press was
 *  claimed when it went down. */
bool lcdKeyReport(uint8_t key, bool down);

/** Whether `key` is held now (as last reported). */
bool lcdKeyDown(uint8_t key);

/** Consumer: deliver every edge of a claimed key to the calling task as an aux
 *  message on `auxPort` (register the handler with itsOnAux first). One
 *  subscription per task; subscribing again moves it to the new port. Returns
 *  false when the subscriber table is full or the lcd task is not running. */
bool lcdKeySubscribe(uint16_t auxPort);

/** Consumer: stop delivering edges to the calling task. */
void lcdKeyUnsubscribe(void);

/** Claim (`claim` true) or release a claim on `key`. Claims count: the key is
 *  claimed while more claims than releases have been made. Releasing a key
 *  that is held delivers its up edge, so no subscriber is left keyed. */
void lcdKeyClaim(uint8_t key, bool claim);

#ifdef __cplusplus
}
#endif

#endif /* SPANGAP_LCD_KEYS_H */
