/**
 * lcd_touch.cpp — the component-owned touch controller (CONFIG_LCD_TOUCH_*).
 *
 * Standard I2C touch parts (FT5x06 family, GT911) driven through esp_lcd_touch
 * and brought up from Kconfig exactly like the panel (lcd_panel.cpp): a board
 * with one of these contributes no touch C code, only pins. Bespoke touch — a
 * controller sampled on a board task to share its bus with other board
 * peripherals (the T-Deck), or a part esp_lcd_touch has no driver for — still
 * comes through lcd_input.h's touch_read, which touchReadCb falls back to
 * whenever no controller is selected here.
 *
 * SAMPLED OFF THE LCD TASK, AND EDGES ARE QUEUED. Both halves of that matter,
 * and a keyboard is what proves it: type quickly and every key after the first
 * lands while the lcd task is still drawing the one before it. A reader that
 * asks the controller "is a finger down?" from the lcd task can only answer
 * between renders, so a tap that begins and ends inside one render is a tap
 * that never happened. So a small task of our own does the I2C — woken by the
 * INT, re-reading at kTrackMs while a finger is down — and turns what it sees
 * into EVENTS: every press and every release goes into a queue that the lcd
 * task drains one per read, each becoming a real LVGL press or release, however
 * far behind it got. The live points are latched alongside for the tracking
 * case (a drag is a stream, not an edge), and are reported whenever the queue
 * is empty. A burst of five taps during one long render arrives as five taps.
 *
 * A LIFT HAS TO BE SEEN SEVERAL TIMES BEFORE IT IS BELIEVED (kLiftReads). The
 * glass drops the occasional sample, and one empty read is not a finger going
 * away — it is a finger the controller missed. Anything reading a whole stroke
 * would otherwise get it in pieces.
 *
 * How the glass is laminated onto the panel is board wiring like any other pin:
 * CONFIG_LCD_TOUCH_SWAP_XY / _MIRROR_X / _MIRROR_Y correct the raw point into
 * the panel's native frame before the panel's own rotation is applied to it.
 * (CONFIG_LCD_MIRROR_X/Y are the panel's — they move pixels and points
 * together, so they cannot straighten touch on their own.)
 *
 * Also home to the component-level touch request key, controller or not:
 * `lcd.multi_touch` (ephemeral, no `s.`) — a consumer sets it truthy while it
 * wants multi-finger reads + gestures (maps does, around its pinch-zoom) and
 * clears it on the way out; the subscription flips lcdTouchSetMultipoint().
 *
 * The bus: the component creates the CONFIG_LCD_TOUCH_I2C_PORT master bus
 * itself, so the touch controller must be that port's only creator — unless the
 * board brought that bus up first for the other chips on those wires and says
 * so with CONFIG_LCD_TOUCH_I2C_ADOPT, in which case the controller is added to
 * it as one more device.
 *
 * In standby (sys.standby truthy) reads are gated off — the INT still fires,
 * the sample is discarded — so a resting finger cannot hold the device awake;
 * waking is the board's button, unless `s.lcd.wake_on_touch` says the glass
 * wakes it too (see the block over touchWakeArm).
 */
#include "lcd_internal.h"
#include "i2c_helper.h"     /* SPANGAP_I2C_PULLUP (shared bus wiring policy) */
#include "log.h"
#include "storage.h"

#include "sdkconfig.h"

#include <cstdio>
#include <cstdlib>

#if CONFIG_LCD_TOUCH_CONTROLLER_FT5X06 || CONFIG_LCD_TOUCH_CONTROLLER_GT911
#define LCD_TOUCH_CTL 1
#include "pm.h"             /* light-sleep wake source for the standby touch */
#include "compat.h"         /* spawnTask — the sampler's stack goes in PSRAM */
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"    /* ISR-safe intr disable (the driver call takes a lock) */
#include "esp_attr.h"
#include "esp_sleep.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#if CONFIG_LCD_TOUCH_CONTROLLER_FT5X06
#include "esp_lcd_touch_ft5x06.h"
#else
#include "esp_lcd_touch_gt911.h"
#endif
#else
#define LCD_TOUCH_CTL 0
#endif

#if LCD_TOUCH_CTL

static esp_lcd_touch_handle_t s_tp = nullptr;   /* null until the probe succeeds */
static bool                   s_asleep = false; /* sys.standby: reads gated */

/* ---- the sampler ----
 * kTrackMs is the re-read cadence while a finger is down (a drag's smoothness);
 * with none down the task sleeps on the INT and costs nothing. The queue holds
 * press/release EDGES only — 16 of them is four fast taps' worth of backlog,
 * far past any render, and a full queue drops the oldest rather than the newest
 * so a burst reads as its own tail rather than freezing on its head. */
static const int          kTrackMs   = 10;
/* Reads with no finger in a row before it counts as lifted — see the block over
 * its use in touchSamplerTask. Three of them at kTrackMs is 30 ms. */
static const int          kLiftReads = 3;
static const int          kEventQLen = 16;
struct TouchEdge { int16_t x, y; bool down; };
static QueueHandle_t      s_edges     = nullptr;   /* sampler → lcd task */
static TaskHandle_t       s_sampler   = nullptr;
/* The live sample, for the tracking case. Written by the sampler, read by the
 * lcd task under the same spinlock — a whole multi-finger sample at a time, so
 * a gesture handler never sees two fingers from different moments. */
static portMUX_TYPE       s_liveMux   = portMUX_INITIALIZER_UNLOCKED;
static int16_t            s_liveX[LCD_TOUCH_MAXPTS], s_liveY[LCD_TOUCH_MAXPTS];
static int                s_liveN     = 0;

/* ---- wake on touch (s.lcd.wake_on_touch) ----
 * Whether the glass wakes the device, as the board's button does. It is a
 * board-shaped question, not a taste: a deck that lives in a pocket wants a
 * real button to be the only way back (default off, and the key is read
 * fresh at each standby, so nothing is cached from before the operator
 * changed their mind); a handheld whose button is round the back is expected
 * to wake where you touch it (default on). The default is the BOARD's — it
 * ships the settings row that seeds the key.
 *
 * Armed only while asleep, because it costs an INT line's worth of light-sleep
 * wake: LOW_LEVEL, since edges are invisible while the GPIO clock is gated —
 * and a level ISR re-fires for as long as the controller holds INT down, so
 * touchWakeISR silences the pin on first fire and the standby read re-enables
 * it. The same two traps the boards' wake buttons carry, for the same reasons.
 *
 * The controller itself is NOT put to sleep either way: on a board with no
 * touch RESET routed (the usual case — CONFIG_LCD_TOUCH_RST_PIN is -1) an
 * FT5x06 in hibernate can only be brought back by cycling its supply, so its
 * scanning current is paid for as long as the rail is up, whatever this says. */
static bool                   s_wakeArmed = false;
static int                    s_wakeLevel = 0;   /* INT level that means "finger" (see touchWakeArm) */
/* The finger that woke the device is swallowed until it lifts — waking is all
 * it does, exactly as a board's wake press is absorbed rather than clicking
 * what the dark screen happened to be showing. */
static bool                   s_wakeAbsorb = false;

static bool wakeOnTouch(void) { return storageGetInt("s.lcd.wake_on_touch", 0) != 0; }

#if CONFIG_LCD_TOUCH_CONTROLLER_FT5X06
/* ---- how hard a touch has to be (s.lcd.touch_sens) ----
 * The FT5x06 keeps its valid-touch detect threshold in ID_G_THGROUP: the
 * LOWER it is, the lighter a touch registers. The slider runs the way a person
 * reads it (right = lighter), so the map inverts it — and 50 lands on the 70
 * the driver writes at its own init, which is what every board felt like
 * before the dial existed. The ends are deliberately wide: they are there to
 * be tried on real glass, and a value that turns out to be useless is one
 * write away from the browser or the CLI.
 *
 * The write is a plain register poke on the panel-io handle the driver reads
 * through — kept from the probe for exactly this — on the lcd task, the task
 * that also does the touch reads. */
#define FT5X06_REG_THGROUP  0x80
static const int  kSensDefault = 50;
static const int  kThAtZero    = 120;   /* threshold at sens 0; sens 100 → 20 */
static esp_lcd_panel_io_handle_t s_tio = nullptr;

static void touchSensApply(int sens) {
    if (!s_tio) return;
    if (sens < 0) sens = 0; else if (sens > 100) sens = 100;
    uint8_t th = (uint8_t)(kThAtZero - sens);
    if (esp_lcd_panel_io_tx_param(s_tio, FT5X06_REG_THGROUP, &th, 1) != ESP_OK)
        warn("touch: sensitivity write failed\n");
    else
        info("touch: sensitivity %d (threshold %u)\n", sens, (unsigned)th);
}
#endif

/* The INT wakes the SAMPLER, not the lcd task: the read has to happen whether
 * or not the lcd task is mid-render, which is the whole point of having a task
 * for it. IRAM: the shared ISR service is installed with ESP_INTR_FLAG_IRAM. */
static void IRAM_ATTR touchISR(void*) {
    if (!s_sampler) return;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_sampler, &hp);
    portYIELD_FROM_ISR(hp);
}

/* First fire of the LOW_LEVEL wake: silence the pin (see the storm note above),
 * then the normal notify. LL register write, not gpio_intr_disable() — the
 * driver call takes a non-ISR spinlock. */
static void IRAM_ATTR touchWakeISR(void* arg) {
    gpio_ll_intr_disable(&GPIO, (gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN);
    touchISR(arg);
}

/* Light-sleep backstop (IDLE-task context, every light-sleep exit): a wake
 * caused by something else entirely — a radio's DIO, the board's button —
 * leaves INT high and is ignored; a finger holding it low has the lcd task
 * read the controller even if the level interrupt was missed on the way out of
 * sleep. */
static void touchSleepWake(int cause) {
    if (cause != ESP_SLEEP_WAKEUP_GPIO || !s_asleep || !s_wakeArmed) return;
    if (gpio_get_level((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN) != s_wakeLevel) return;
    if (s_sampler) xTaskNotifyGive(s_sampler);
}

/* Swap the INT between its awake wiring (ANYEDGE, straight to the sampler) and
 * the standby wake source (level-triggered, silencing ISR). The level is the
 * OPPOSITE of whatever the line rests at with no finger on the glass: INT
 * polarity is part- and sub-revision-dependent (which is why the awake wiring
 * is ANYEDGE), and arming the resting level would wake the device forever.
 * Lcd task. */
static void touchWakeArm(bool on) {
    if (on == s_wakeArmed || CONFIG_LCD_TOUCH_INT_PIN < 0) return;
    gpio_isr_handler_remove((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN);
    gpio_isr_handler_add((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN,
                         on ? touchWakeISR : touchISR, nullptr);
    if (on) {
        s_wakeLevel = gpio_get_level((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN) ? 0 : 1;
        pmGpioWakeEnable(CONFIG_LCD_TOUCH_INT_PIN,
                         s_wakeLevel ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    } else {
        pmGpioWakeDisable(CONFIG_LCD_TOUCH_INT_PIN);
        gpio_set_intr_type((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN, GPIO_INTR_ANYEDGE);
    }
    gpio_intr_enable((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN);
    s_wakeArmed = on;
}

/* One I2C read → the panel's native frame. How the glass is laminated onto the
 * panel is corrected here, once, so everything downstream — the queued edges,
 * the live latch, the gesture handlers — speaks native coordinates and only the
 * panel's own rotation is left to apply. Sampler task. */
static bool touchSample(int16_t* xs, int16_t* ys, int* count) {
    *count = 0;
    if (!s_tp) return false;
    /* A bus glitch loses one sample and the next read picks the finger back up,
     * so this is a warning, not an error — reported on the failing edge only,
     * since a wedged bus fails on every read. */
    static bool readFailed = false;
    if (esp_lcd_touch_read_data(s_tp) != ESP_OK) {
        if (!readFailed) warn("touch: read failed\n");
        readFailed = true;
        return false;
    }
    readFailed = false;
    esp_lcd_touch_point_data_t pt[LCD_TOUCH_MAXPTS] = {};
    uint8_t cnt = 0;
    esp_lcd_touch_get_data(s_tp, pt, &cnt, LCD_TOUCH_MAXPTS);
    int n = cnt > LCD_TOUCH_MAXPTS ? LCD_TOUCH_MAXPTS : cnt;
    for (int i = 0; i < n; i++) {
        int x = pt[i].x, y = pt[i].y;
#if CONFIG_LCD_TOUCH_SWAP_XY
        { int t = x; x = y; y = t; }
#endif
#if CONFIG_LCD_TOUCH_MIRROR_X
        x = (CONFIG_LCD_NATIVE_WIDTH  - 1) - x;
#endif
#if CONFIG_LCD_TOUCH_MIRROR_Y
        y = (CONFIG_LCD_NATIVE_HEIGHT - 1) - y;
#endif
        xs[i] = (int16_t)x;
        ys[i] = (int16_t)y;
    }
    *count = n;
    return true;
}

/* A press or a release the lcd task has yet to see. The queue is the memory of
 * what happened while it was busy; when it is full the OLDEST goes, because the
 * finger the operator has on the glass now matters more than the one they had
 * five taps ago. */
static void pushEdge(int16_t x, int16_t y, bool down) {
    if (!s_edges) return;
    TouchEdge e = { x, y, down };
    if (xQueueSend(s_edges, &e, 0) != pdTRUE) {
        TouchEdge drop;
        xQueueReceive(s_edges, &drop, 0);
        xQueueSend(s_edges, &e, 0);
    }
}

/* The sampler. Sleeps on the INT with nothing down, re-reads every kTrackMs
 * while a finger is, and runs above the lcd task so a render never delays it.
 * Everything it learns leaves by one of two doors: an EDGE on the queue (which
 * the lcd task turns into an LVGL press or release, however late) or the LIVE
 * latch (which it reports while nothing is queued). */
static void touchSamplerTask(void*) {
    bool down = false;
    int  emptyReads = 0;      /* reads with no finger in a row; see kLiftReads */
    int16_t xs[LCD_TOUCH_MAXPTS], ys[LCD_TOUCH_MAXPTS];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, down ? pdMS_TO_TICKS(kTrackMs) : portMAX_DELAY);
        int n = 0;

        if (s_asleep) {
            /* Dark: nothing is ever reported, and whatever was in flight when
             * the screen went off is dropped rather than delivered on the far
             * side of a sleep. Where the glass is a wake source a real finger
             * clears the key — and only a real one, since the INT can fire for
             * a glitch. */
            down = false;
            emptyReads = 0;
            taskENTER_CRITICAL(&s_liveMux);
            s_liveN = 0;
            taskEXIT_CRITICAL(&s_liveMux);
            if (s_edges) xQueueReset(s_edges);
            if (!s_wakeArmed) continue;
            gpio_intr_enable((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN);   /* the wake ISR silenced it */
            if (!touchSample(xs, ys, &n) || n == 0) continue;
            s_wakeAbsorb = true;
            storageSet("sys.standby", 0);       /* the board takes it from here */
            continue;
        }

        if (!touchSample(xs, ys, &n)) continue;

        /* The finger that woke the device: it wakes and does nothing else, so
         * it is watched (to know when it lifts) but never reported. */
        if (s_wakeAbsorb) {
            down = n > 0;
            if (n == 0) s_wakeAbsorb = false;
            continue;
        }

        /* ONE EMPTY READ IS NOT A LIFT. A capacitive panel misses the odd
         * sample — a fast-moving finger lightens its contact, a read lands
         * between the controller's own scans — and a finger is only gone once
         * it has been missing for kLiftReads reads together. Until then the
         * live latch holds its last position and no edge is queued.
         *
         * A tap barely notices either way. What does is anything that reads a
         * WHOLE STROKE rather than its ends: taking the first empty read as a
         * release chops one drag into two, and the two halves are not a gesture
         * that was made. Thirty milliseconds of grace is far below the gap
         * between a real lift and the next touch, and far above a dropout. */
        if (n == 0 && down && ++emptyReads < kLiftReads) continue;
        if (n > 0) emptyReads = 0;

        taskENTER_CRITICAL(&s_liveMux);
        for (int i = 0; i < n; i++) { s_liveX[i] = xs[i]; s_liveY[i] = ys[i]; }
        s_liveN = n;
        taskEXIT_CRITICAL(&s_liveMux);

        bool edge = false;
        if (n > 0 && !down)       { down = true;  pushEdge(xs[0], ys[0], true);  edge = true; }
        else if (n == 0 && down)  { down = false; pushEdge(0, 0, false);         edge = true; }
        /* Bump per sample while a finger is down (tracking), and on every edge.
         * lcdTouchPoll coalesces, so this is one hop per gesture, not per read. */
        if (edge || n > 0) lcdTouchPoll();
    }
}

static void touchCtlBringup(void) {
    i2c_master_bus_handle_t i2c = nullptr;
#if CONFIG_LCD_TOUCH_I2C_ADOPT
    /* The board's bus. It exists already — a board service runs in the start
     * band, ahead of the display — so a failure here is a board that named the
     * wrong controller, not a race to retry. */
    if (i2c_master_get_bus_handle(CONFIG_LCD_TOUCH_I2C_PORT, &i2c) != ESP_OK) {
        warn("touch: no i2c bus on port %d to share\n", CONFIG_LCD_TOUCH_I2C_PORT);
        return;
    }
#else
    i2c_master_bus_config_t bcfg = {};
    bcfg.i2c_port                     = CONFIG_LCD_TOUCH_I2C_PORT;
    bcfg.sda_io_num                   = (gpio_num_t)CONFIG_LCD_TOUCH_I2C_SDA;
    bcfg.scl_io_num                   = (gpio_num_t)CONFIG_LCD_TOUCH_I2C_SCL;
    bcfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bcfg.glitch_ignore_cnt            = 7;
    bcfg.flags.enable_internal_pullup = SPANGAP_I2C_PULLUP;
    if (i2c_new_master_bus(&bcfg, &i2c) != ESP_OK) {
        warn("touch: i2c bus init failed\n");
        return;
    }
#endif

    esp_lcd_touch_config_t tcfg = {};
    /* esp_lcd_touch stays at IDENTITY (native coords): the glass's own mounting
     * is corrected in lcdTouchCtlRead (CONFIG_LCD_TOUCH_SWAP_XY / _MIRROR_*)
     * and touchReadCb then orients the points with the same CONFIG_LCD_ROTATION
     * it applies to the pixels — so the maxes here are the native ranges, and
     * the driver's own flags stay off (its mirror is x_max - x, one pixel out
     * at both ends). */
    tcfg.x_max         = CONFIG_LCD_NATIVE_WIDTH;
    tcfg.y_max         = CONFIG_LCD_NATIVE_HEIGHT;
    tcfg.rst_gpio_num  = (gpio_num_t)CONFIG_LCD_TOUCH_RST_PIN;   /* -1 = none */
    tcfg.int_gpio_num  = (gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN;
    tcfg.flags.swap_xy  = 0;
    tcfg.flags.mirror_x = 0;
    tcfg.flags.mirror_y = 0;

#if CONFIG_LCD_TOUCH_CONTROLLER_FT5X06
    const char*   name    = "FT5x06";
    /* The FT5x06 family's one fixed address. */
    const uint8_t addrs[] = { ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS };
    const int     cmdBits = 8;
#else
    const char*   name    = "GT911";
    /* The GT911 latches its address from the INT level at power-on
     * (low -> 0x5D, high -> 0x14); with no dedicated reset the level is
     * board-dependent, so probe both. */
    const uint8_t addrs[] = { ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
                              ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP };
    const int     cmdBits = 16;
#endif

    /* Driver-tag noise control: a failed read or probe logs several driver
     * lines for one event, and the one worth keeping is our own warn(). The
     * wrong-address half of a probe and a not-yet-powered controller fail this
     * way as a matter of course. */
    logRule("FT5x06: ", 'N');
    logRule("GT911: ", 'N');
    logRule("panel_io_i2c_rx_buffer", 'N');

    /* The controller runs its own firmware, usually off a gated power rail;
     * right after a cold power-on it can miss the first probe. Give the rail
     * time and retry before declaring it absent. */
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(150));
        for (uint8_t addr : addrs) {
            esp_lcd_panel_io_handle_t tio = nullptr;
            /* Built by hand: the driver CONFIG macros use out-of-order
             * designated initializers, a hard error in C++. */
            esp_lcd_panel_io_i2c_config_t io_cfg = {};
            io_cfg.dev_addr            = addr;
            io_cfg.scl_speed_hz        = CONFIG_LCD_TOUCH_I2C_KHZ * 1000;
            io_cfg.control_phase_bytes = 1;
            io_cfg.dc_bit_offset       = 0;
            io_cfg.lcd_cmd_bits        = cmdBits;
            io_cfg.flags.disable_control_phase = 1;
            if (esp_lcd_new_panel_io_i2c_v2(i2c, &io_cfg, &tio) != ESP_OK) continue;

            esp_lcd_touch_handle_t tp = nullptr;
#if CONFIG_LCD_TOUCH_CONTROLLER_FT5X06
            esp_err_t err = esp_lcd_touch_new_i2c_ft5x06(tio, &tcfg, &tp);
#else
            esp_err_t err = esp_lcd_touch_new_i2c_gt911(tio, &tcfg, &tp);
#endif
            if (err == ESP_OK) {
                info("touch: %s ready @ 0x%02X\n", name, addr);
#if CONFIG_LCD_TOUCH_CONTROLLER_FT5X06
                s_tio = tio;      /* kept for the sensitivity register */
#endif
                char tch[20];
                snprintf(tch, sizeof(tch), "%s @ 0x%02X", name, addr);
                storageSet("lcd.touch", tch);   /* for a board's Hardware pane */
                /* esp_lcd_touch configured the INT pin as input; take it
                 * interrupt-driven (ANYEDGE — polarity is part-dependent, and
                 * a redundant edge just costs one empty read). The edge wakes
                 * the lcd task, which reads the (event-mode) touch indev via
                 * lcdTouchCtlRead. */
                s_tp = tp;
                /* The sampler owns every read from here on. Above the lcd task
                 * (prio 2) on purpose: a tap that lands mid-render has to be
                 * seen while it is still under the finger. */
                s_edges = xQueueCreate(kEventQLen, sizeof(TouchEdge));
                /* PSRAM stack, like the lcd task's: this one reads a touch
                 * controller over I2C and pushes small structs, never touches
                 * flash, and internal RAM is the resource everything else on
                 * this chip is short of. */
                s_sampler = spawnTask(touchSamplerTask, "lcdtouch", 4096, nullptr,
                                      6, 1, STACK_PSRAM);
                gpio_set_intr_type((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN, GPIO_INTR_ANYEDGE);
                gpio_isr_handler_add((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN, touchISR, nullptr);
                gpio_intr_enable((gpio_num_t)CONFIG_LCD_TOUCH_INT_PIN);
                pmOnLightSleepWake(touchSleepWake);   /* backstop for wake-on-touch */
                return;
            }
            esp_lcd_panel_io_del(tio);   /* free and try the next address / attempt */
        }
    }
    warn("touch: %s not found after 4 attempts\n", name);
    storageSet("lcd.touch", "not found");
}

#endif  /* LCD_TOUCH_CTL */

bool lcdTouchCtlConfigured(void) { return LCD_TOUCH_CTL != 0; }

/* Called from lcdLvglInit on the lcd task, after the panel (and the shared GPIO
 * ISR service) are up and after the board input HAL's own init. Storage is up
 * (lcdInit runs inside spangapInit), so the subscriptions dispatch here. */
void lcdTouchCtlInit(void) {
    /* The multi-touch request key is component-level — it drives
     * lcdTouchSetMultipoint whether touch comes from this controller or from a
     * board HAL's touch_read. */
    NOW_AND_ON_CHANGE("lcd.multi_touch", { lcdTouchSetMultipoint(atoi(val) != 0); });
#if LCD_TOUCH_CTL
    /* Standby both gates the reads and decides whether the glass is a wake
     * source; the key is read here, at the transition, so a change made while
     * the screen was on is in force the moment it goes off. */
    storageSubscribeChanges("sys.standby", ON_CHANGE {
        s_asleep = atoi(val) != 0;
        touchWakeArm(s_asleep && wakeOnTouch());
    });
    touchCtlBringup();
#endif
#if CONFIG_LCD_TOUCH_CONTROLLER_FT5X06
    /* After the bring-up: the driver writes its own threshold at init, and the
     * dial's whole job is to land on top of that. The default is seeded here
     * rather than from the settings row, so the value exists before the
     * generated defaults are (they are seeded after spangapInit, and this runs
     * inside it) — a silent default fires no subscription, hence the read the
     * macro does for itself. */
    storageDefault("s.lcd.touch_sens", kSensDefault);
    NOW_AND_ON_CHANGE("s.lcd.touch_sens", { touchSensApply(atoi(val)); });
#endif
}

/* Whether an edge is still waiting. touchReadCb asks after every read and forces
 * an immediate follow-up when it is true, so a backlog drains inside one pass of
 * the lcd loop — press, release, press, release — instead of one edge per render. */
bool lcdTouchCtlPending(void) {
#if LCD_TOUCH_CTL
    return s_edges && uxQueueMessagesWaiting(s_edges) > 0;
#else
    return false;
#endif
}

/* touchReadCb's first source: true = this module owns touch (write the sample,
 * count 0 when no finger / asleep / controller absent), false = no controller
 * is selected and the board HAL's touch_read should be consulted instead.
 *
 * Queued edges first, one per call — LVGL needs a press and its release in
 * SEPARATE reads to make a click, so handing over more than one edge per read
 * would collapse a tap. Only when nothing is queued does the live sample stand
 * in, which is what carries a drag (and the second finger of a pinch). */
bool lcdTouchCtlRead(lcd_raw_pt_t* pts, int max, int* count) {
#if LCD_TOUCH_CTL
    *count = 0;
    if (!s_tp) return true;
    if (s_asleep) return true;          /* dark: nothing is reported, queue and all */
    TouchEdge e;
    if (s_edges && xQueueReceive(s_edges, &e, 0) == pdTRUE) {
        if (e.down && max > 0) { pts[0].x = e.x; pts[0].y = e.y; *count = 1; }
        return true;
    }
    taskENTER_CRITICAL(&s_liveMux);
    int n = s_liveN;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) { pts[i].x = s_liveX[i]; pts[i].y = s_liveY[i]; }
    taskEXIT_CRITICAL(&s_liveMux);
    *count = n;
    return true;
#else
    (void)pts; (void)max; (void)count;
    return false;
#endif
}
