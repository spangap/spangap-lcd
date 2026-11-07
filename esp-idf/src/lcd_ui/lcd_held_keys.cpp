/**
 * lcd_held_keys.cpp — the held-key API (include/lcd_keys.h).
 *
 *   board task   lcdKeyReport(k, down)   atomics + notify the lcd task
 *   any task     lcdKeyClaim / lcdKeySubscribe / lcdKeyUnsubscribe
 *   lcd task     lcdKeysService()        reconciles every subscriber, sends
 *                                        each edge as an aux (timeout 0)
 *
 * Everything a caller needs answered at once — declared, down, claimed — is an
 * atomic, so lcdKeyReport returns on the calling task without waiting for
 * anybody. The edges themselves are not queued: the lcd task is woken with a
 * plain task notification and works out what each subscriber is owed from the
 * state (a press counter per key, the held and claimed bits). Nothing can be
 * dropped on the way in, a press that came and went before the lcd task looked
 * is still delivered as a down and an up, and an aux send that fails (the
 * subscriber's inbox is full) is simply owed and retried on a short timer.
 *
 * A press belongs to the claim it began under: lcdKeyReport marks it GRABBED
 * when the key was claimed at the moment it went down, only grabbed presses
 * reach subscribers, and a press that began unclaimed (and was typed) stays the
 * board's even if a claim arrives while it is held. A grabbed press that loses
 * its last claim while still held is delivered up at once.
 *
 * The subscriber table is written by subscribing tasks under a spinlock and
 * read by the lcd task as a snapshot; the delivery state per subscriber is the
 * lcd task's alone and is reset whenever its slot changes hands; a task that
 * resubscribes on another port keeps it, so its held keys still come up.
 */
#include "../../include/lcd_keys.h"   /* the public header: src/lcd_ui/lcd_keys.h is the on-screen keyboard widget */
#include "lcd_internal.h"

#include "its.h"
#include "log.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstring>

namespace {

constexpr int      kMaxSubs      = 8;
constexpr uint64_t kRetryUs      = 20 * 1000;   /* an owed aux is retried this soon */
constexpr int64_t  kDropUs       = 1000 * 1000; /* 1 s of nothing but failed sends drops the subscriber */

std::atomic<uint32_t> s_declared{0};            /* bit k: the board reports key k */
std::atomic<uint32_t> s_down{0};                /* bit k: held, as last reported */
std::atomic<uint32_t> s_grabbed{0};             /* bit k: the current press began claimed */
std::atomic<uint32_t> s_presses[LCD_KEY_COUNT]; /* grabbed presses so far */
std::atomic<int32_t>  s_claims[LCD_KEY_COUNT];
std::atomic<bool>     s_dirty{false};           /* the lcd task has reconciling to do */
std::atomic<void (*)(uint8_t, bool)> s_claimWatch{nullptr};   /* the board's claim watcher */

struct Sub {                  /* guarded by s_subMux */
    TaskHandle_t task;
    uint16_t     port;
    uint32_t     gen;         /* bumped on every (re)subscribe */
};
portMUX_TYPE s_subMux = portMUX_INITIALIZER_UNLOCKED;
Sub          s_subs[kMaxSubs];
uint32_t     s_subGen = 0;

struct SubState {             /* lcd task only */
    TaskHandle_t task;
    uint32_t     gen;
    uint8_t      keyed;                     /* bit k: a down was delivered, no up yet */
    uint32_t     seen[LCD_KEY_COUNT];       /* press count already delivered */
    int64_t      failingSince;              /* esp_timer time of the first failed send
                                               since the last good pass; 0 = none */
};
SubState s_state[kMaxSubs];

esp_timer_handle_t s_retryTimer = nullptr;

inline bool validKey(uint8_t k) { return k < LCD_KEY_COUNT; }

void kick() {
    s_dirty.store(true);
    if (lcdTaskHandle) xTaskNotifyGive(lcdTaskHandle);
}

void retryCb(void*) { kick(); }

void scheduleRetry() {
    if (!s_retryTimer) {
        esp_timer_create_args_t a = {};
        a.callback = retryCb;
        a.name     = "lcdkeys";
        if (esp_timer_create(&a, &s_retryTimer) != ESP_OK) { s_retryTimer = nullptr; return; }
    }
    if (!esp_timer_is_active(s_retryTimer)) esp_timer_start_once(s_retryTimer, kRetryUs);
}

bool sendEdge(TaskHandle_t task, uint16_t port, uint8_t key, bool down) {
    lcd_key_edge_t e{ key, (uint8_t)(down ? 1 : 0) };
    return itsSendAuxByTaskHandle(task, port, &e, sizeof(e), 0);
}

}  // namespace

extern "C" {

void lcdKeyDeclare(uint8_t key) {
    if (validKey(key)) s_declared.fetch_or(1u << key);
}

void lcdKeyUndeclare(uint8_t key) {
    if (validKey(key)) s_declared.fetch_and(~(1u << key));
}

bool lcdKeyDeclared(uint8_t key) {
    return validKey(key) && (s_declared.load() & (1u << key));
}

static const char* s_labels[LCD_KEY_COUNT] = { "Talk", "Space" };

void lcdKeySetLabel(uint8_t key, const char* label) {
    if (validKey(key) && label) s_labels[key] = label;
}

const char* lcdKeyLabel(uint8_t key) {
    return validKey(key) ? s_labels[key] : "";
}

bool lcdKeyClaimed(uint8_t key) {
    return validKey(key) && s_claims[key].load() > 0;
}

bool lcdKeyDown(uint8_t key) {
    return validKey(key) && (s_down.load() & (1u << key));
}

bool lcdKeyReport(uint8_t key, bool down) {
    if (!validKey(key)) return false;
    const uint32_t bit = 1u << key;
    if (down) {
        const bool claimed = s_claims[key].load() > 0;
        /* Held and grabbed before the press is counted: the lcd task acts on a
         * new count, and must find the key it describes already down. */
        if (claimed) s_grabbed.fetch_or(bit);
        else         s_grabbed.fetch_and(~bit);
        s_down.fetch_or(bit);
        if (claimed) { s_presses[key].fetch_add(1); kick(); }
        return claimed;
    }
    s_down.fetch_and(~bit);
    const bool grabbed = s_grabbed.fetch_and(~bit) & bit;
    if (grabbed) kick();
    return grabbed;
}

bool lcdKeySubscribe(uint16_t auxPort) {
    if (!lcdTaskHandle) return false;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bool ok = false;
    taskENTER_CRITICAL(&s_subMux);
    int slot = -1;
    for (int i = 0; i < kMaxSubs; i++) if (s_subs[i].task == me) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < kMaxSubs; i++) if (!s_subs[i].task) { slot = i; break; }
    if (slot >= 0) {
        s_subs[slot].task = me;
        s_subs[slot].port = auxPort;
        s_subs[slot].gen  = ++s_subGen;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_subMux);
    if (ok) kick();
    else    warn("key subscribers full (%d)\n", kMaxSubs);
    return ok;
}

void lcdKeyUnsubscribe(void) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bool found = false;
    taskENTER_CRITICAL(&s_subMux);
    for (int i = 0; i < kMaxSubs; i++)
        if (s_subs[i].task == me) { s_subs[i] = Sub{}; found = true; }
    taskEXIT_CRITICAL(&s_subMux);
    if (found) kick();
}

void lcdKeyOnClaimChange(void (*cb)(uint8_t key, bool claimed)) {
    s_claimWatch.store(cb);
}

void lcdKeyClaim(uint8_t key, bool claim) {
    if (!validKey(key)) return;
    void (*watch)(uint8_t, bool) = s_claimWatch.load();
    if (claim) {
        if (s_claims[key].fetch_add(1) == 0 && watch) watch(key, true);
        return;
    }
    int32_t c = s_claims[key].load();
    while (c > 0 && !s_claims[key].compare_exchange_weak(c, c - 1)) {}
    if (c == 1) {
        kick();   /* the last claim went: a held grabbed press is owed its up */
        if (watch) watch(key, false);
    }
}

}  // extern "C"

/* Lcd task, every loop pass: deliver what each subscriber is owed. A cheap no-op
 * unless something was reported, claimed or subscribed since the last pass. */
void lcdKeysService(void) {
    if (!s_dirty.exchange(false)) return;

    Sub snap[kMaxSubs];
    taskENTER_CRITICAL(&s_subMux);
    memcpy(snap, s_subs, sizeof(snap));
    taskEXIT_CRITICAL(&s_subMux);

    uint32_t presses[LCD_KEY_COUNT];
    bool     claimed[LCD_KEY_COUNT];
    for (int k = 0; k < LCD_KEY_COUNT; k++) {
        presses[k] = s_presses[k].load();
        claimed[k] = s_claims[k].load() > 0;
    }
    const uint32_t down    = s_down.load();
    const uint32_t grabbed = s_grabbed.load();

    /* A press seen for the first time is user input like any other. */
    static uint32_t s_activitySeen[LCD_KEY_COUNT];
    for (int k = 0; k < LCD_KEY_COUNT; k++)
        if (presses[k] != s_activitySeen[k]) { s_activitySeen[k] = presses[k]; lcdActivity(); }

    bool owed = false;
    for (int i = 0; i < kMaxSubs; i++) {
        SubState& st = s_state[i];
        const Sub& sb = snap[i];
        if (st.task == sb.task && st.gen != sb.gen) {
            /* The same task resubscribed (a new port): its delivery carries
             * over, so a key it holds down still gets its up, on the new port. */
            st.gen = sb.gen;
            st.failingSince = 0;
        } else if (st.task != sb.task) {
            st = SubState{};
            st.task = sb.task;
            st.gen  = sb.gen;
            for (int k = 0; k < LCD_KEY_COUNT; k++) st.seen[k] = presses[k];
        }
        if (!sb.task) continue;

        bool failed = false;
        for (int k = 0; k < LCD_KEY_COUNT && !failed; k++) {
            const uint8_t bit = (uint8_t)(1u << k);
            if (st.seen[k] != presses[k]) {
                if (!claimed[k]) {
                    st.seen[k] = presses[k];          /* nobody wants it any more */
                } else {
                    if (st.keyed & bit) {             /* a release we never got to */
                        if (!sendEdge(sb.task, sb.port, (uint8_t)k, false)) { failed = true; break; }
                        st.keyed &= (uint8_t)~bit;
                    }
                    if (!sendEdge(sb.task, sb.port, (uint8_t)k, true)) { failed = true; break; }
                    st.keyed |= bit;
                    st.seen[k] = presses[k];
                }
            }
            const bool want = claimed[k] && (down & grabbed & (1u << k));
            if ((st.keyed & bit) && !want) {
                if (!sendEdge(sb.task, sb.port, (uint8_t)k, false)) { failed = true; break; }
                st.keyed &= (uint8_t)~bit;
            }
        }
        if (!failed) { st.failingSince = 0; continue; }
        /* Measured in time, not passes: the lcd task services far more often
         * than the retry timer whenever anything else wakes it. */
        const int64_t now = esp_timer_get_time();
        if (!st.failingSince) st.failingSince = now;
        if (now - st.failingSince >= kDropUs) {
            warn("key subscriber on aux port %u not taking edges; dropped\n", (unsigned)sb.port);
            taskENTER_CRITICAL(&s_subMux);
            if (s_subs[i].task == sb.task && s_subs[i].gen == sb.gen) s_subs[i] = Sub{};
            taskEXIT_CRITICAL(&s_subMux);
            continue;
        }
        owed = true;
    }
    if (owed) { s_dirty.store(true); scheduleRetry(); }
}
