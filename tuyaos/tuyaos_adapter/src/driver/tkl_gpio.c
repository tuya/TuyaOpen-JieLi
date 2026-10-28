#include "tkl_gpio.h"
#include "tuya_error_code.h"

/* gpio.h uses u32/u8 without including their definitions. */
#include "asm/cpu.h"
#include "gpio.h"

/* WL82 uses a shared wakeup table with two remappable slots; WL83 has per-pin
 * EXTI. Both support rising/falling edges only. */
#if defined(JIELI_SELECTED_CHIP_WL82)
#include "asm/port_waked_up.h"
#elif defined(JIELI_SELECTED_CHIP_WL83)
#include "asm/exti.h"
#else
#error "JIELI tkl_gpio.c needs JIELI_SELECTED_CHIP_WL82 or _WL83 defined"
#endif

/* Tuya and JieLi use the same flat 16-pins-per-port numbering. This is the
 * TKL namespace bound, not proof that every pin is bonded out on a board. */
#define TKL_GPIO_PIN_MAX 128

/* Only EVENT_IO_0 and EVENT_IO_1 are general-purpose remappable slots. */
#define TKL_GPIO_WL82_IRQ_MAX 2

typedef struct {
    TUYA_GPIO_IRQ_CB cb;
    void *arg;
    TUYA_GPIO_IRQ_E mode;
    void *hdl;   /* wl82: the port_wakeup handle. wl83: unused. */
    int exti;    /* wl83: the exti index, which may legitimately be 0. */
    u8 armed;    /* registration is live; NOT deriveable from the values above */
} tkl_gpio_irq_ctx_t;

static tkl_gpio_irq_ctx_t s_irq[TKL_GPIO_PIN_MAX];

/* The vendor pin space is 16 pins per port letter; both chips use that width. */
#define JIELI_PORT_WIDTH 16u

/* Return whether a flat pin number exists in the chip GPIO map. WL83 has sparse
 * C/F ports and USB pairs; WL82 exposes ports A through H. Package bonding is
 * board-specific. tkl_pinmux.c shares this check. */
int jieli_pin_exists(uint32_t pin)
{
#if defined(JIELI_SELECTED_CHIP_WL83)
    uint32_t port = pin / JIELI_PORT_WIDTH;
    uint32_t bit = pin % JIELI_PORT_WIDTH;

    switch (port) {
    case 0: /* A */
    case 1: /* B */
    case 3: /* D */
    case 4: /* E */
        return 1;
    case 2: /* C: PC0..PC13 */
        return bit < 14u;
    case 5: /* F: PF0..PF5 */
        return bit < 6u;
    case 6: /* FUSB: only DPA/DMA are defined */
        return bit < 2u;
    case 7: /* HUSB: only DPB/DMB are defined */
        return bit < 2u;
    default:
        return 0;
    }
#else /* JIELI_SELECTED_CHIP_WL82 */
    return pin < 128u;
#endif
}

static int tkl_gpio_valid(TUYA_GPIO_NUM_E pin_id)
{
    /* Reject holes in WL83's sparse port map. */
    return (pin_id < TUYA_GPIO_NUM_MAX) && (pin_id < TKL_GPIO_PIN_MAX) &&
           jieli_pin_exists((uint32_t)pin_id);
}

/* Both chips expose positive and negative edge triggers. */
static int tkl_irq_edge(TUYA_GPIO_IRQ_E mode, int *edge)
{
    switch (mode) {
    case TUYA_GPIO_IRQ_RISE:
        *edge = EDGE_POSITIVE;
        return 0;
    case TUYA_GPIO_IRQ_FALL:
        *edge = EDGE_NEGATIVE;
        return 0;
    default:
        /* Level and both-edge modes have no equivalent in these SDKs. */
        return -1;
    }
}

static int tkl_gpio_apply_pull(u32 pin, TUYA_GPIO_MODE_E mode)
{
#if defined(JIELI_SELECTED_CHIP_WL82)
    /* WL82 uses 10K pulls selected by enabling one leg and disabling the other. */
    if (mode == TUYA_GPIO_PULLUP) {
        if (gpio_set_pull_down(pin, 0) != 0) return -1;
        if (gpio_set_pull_up(pin, 1) != 0) return -1;
    } else if (mode == TUYA_GPIO_PULLDOWN) {
        if (gpio_set_pull_up(pin, 0) != 0) return -1;
        if (gpio_set_pull_down(pin, 1) != 0) return -1;
    } else {
        if (gpio_set_pull_up(pin, 0) != 0) return -1;
        if (gpio_set_pull_down(pin, 0) != 0) return -1;
    }
#else
    /* WL83 exposes a 10K pull on each leg; disable the unused leg. */
    if (mode == TUYA_GPIO_PULLUP) {
        if (gpio_set_pull_down(pin, GPIO_PULL_DOWN_DISABLE) != 0) return -1;
        if (gpio_set_pull_up(pin, GPIO_PULL_UP_10K) != 0) return -1;
    } else if (mode == TUYA_GPIO_PULLDOWN) {
        if (gpio_set_pull_up(pin, GPIO_PULL_UP_DISABLE) != 0) return -1;
        if (gpio_set_pull_down(pin, GPIO_PULL_DOWN_10K) != 0) return -1;
    } else {
        if (gpio_set_pull_up(pin, GPIO_PULL_UP_DISABLE) != 0) return -1;
        if (gpio_set_pull_down(pin, GPIO_PULL_DOWN_DISABLE) != 0) return -1;
    }
#endif
    return 0;
}

#if defined(JIELI_SELECTED_CHIP_WL82)
/* WL82's handler has no pin argument, so each active slot gets a trampoline. */
static TUYA_GPIO_NUM_E s_wl82_slot_pin[TKL_GPIO_WL82_IRQ_MAX];
/* A separate in-use flag rather than a sentinel pin number: 0 is IO_PORTA_00,
 * a perfectly valid pin, so a zero-initialised array cannot mean "free" and a
 * -1 sentinel would sail past the `pin_id < TKL_GPIO_PIN_MAX` guard below and
 * index s_irq[-1]. */
static u8 s_wl82_slot_used[TKL_GPIO_WL82_IRQ_MAX];

static void tkl_gpio_wl82_dispatch(int slot)
{
    TUYA_GPIO_NUM_E pin_id;

    if (!s_wl82_slot_used[slot]) {
        return;
    }
    pin_id = s_wl82_slot_pin[slot];
    if (pin_id < TKL_GPIO_PIN_MAX && s_irq[pin_id].cb != NULL) {
        s_irq[pin_id].cb(s_irq[pin_id].arg);
    }
}

static void tkl_gpio_wl82_isr_0(void) { tkl_gpio_wl82_dispatch(0); }
static void tkl_gpio_wl82_isr_1(void) { tkl_gpio_wl82_dispatch(1); }

static void (*const s_wl82_isr[TKL_GPIO_WL82_IRQ_MAX])(void) = {
    tkl_gpio_wl82_isr_0,
    tkl_gpio_wl82_isr_1,
};

static OPERATE_RET tkl_gpio_wl82_register(TUYA_GPIO_NUM_E pin_id, int edge)
{
    int slot;
    void *hdl;

    for (slot = 0; slot < TKL_GPIO_WL82_IRQ_MAX; slot++) {
        if (!s_wl82_slot_used[slot]) {
            break;
        }
    }
    if (slot == TKL_GPIO_WL82_IRQ_MAX) {
        /* Every vendor slot is taken. The driver would print its own
         * "PORT_WAKED_ID_NUM to less" and return NULL; catching it here keeps
         * the failure a code the caller can act on. */
        return OPRT_EXCEED_UPPER_LIMIT;
    }

    /* Publish the slot before registering: port_wakeup_reg enables the event
     * as part of the call, so an edge arriving on the way out would otherwise
     * find the slot still marked free and drop the callback. The dispatch
     * checks the used flag, so an interrupt in that window either finds a
     * fully published slot or (before this store) returns harmlessly. */
    s_wl82_slot_pin[slot] = pin_id;
    s_wl82_slot_used[slot] = 1;

    /* One event per slot. EVENT_IO_0 and EVENT_IO_1 each own a separate
     * io-map remap field and a separate enable/edge/pending bit, so two pins
     * must use different events: registering both on EVENT_IO_0 would make the
     * second overwrite the first's remap (so the first pin stops firing) and
     * would make the vendor ISR dispatch BOTH handlers on either pin's edge,
     * because it walks every table entry whose stored event is pended. */
    hdl = port_wakeup_reg(slot == 0 ? EVENT_IO_0 : EVENT_IO_1, (unsigned int)pin_id,
                          (PORT_EDGE_E)edge, s_wl82_isr[slot]);
    if (hdl == NULL) {
        /* The pin is not in the driver's remap table - ports D and F are not
         * listed at all, and A/B/C/E/G/H only partially. Give the slot back. */
        s_wl82_slot_used[slot] = 0;
        return OPRT_NOT_SUPPORTED;
    }

    s_irq[pin_id].hdl = hdl;
    s_irq[pin_id].armed = 1u;
    return OPRT_OK;
}

static void tkl_gpio_wl82_unregister(TUYA_GPIO_NUM_E pin_id)
{
    int slot;

    if (!s_irq[pin_id].armed) {
        return;
    }
    for (slot = 0; slot < TKL_GPIO_WL82_IRQ_MAX; slot++) {
        if (s_wl82_slot_used[slot] && s_wl82_slot_pin[slot] == pin_id) {
            s_wl82_slot_used[slot] = 0;
            break;
        }
    }
    port_wakeup_unreg(s_irq[pin_id].hdl);
    s_irq[pin_id].hdl = NULL;
    s_irq[pin_id].armed = 0u;
}
#else /* JIELI_SELECTED_CHIP_WL83 */
/* wl83 hands the private pointer and the pin back to the callback, so a single
 * dispatcher covers every pin and the pin travels as the private pointer. */
static void tkl_gpio_wl83_dispatch(void *priv, unsigned int gpio)
{
    TUYA_GPIO_NUM_E pin_id = (TUYA_GPIO_NUM_E)(uintptr_t)priv;
    (void)gpio;
    if (pin_id < TKL_GPIO_PIN_MAX && s_irq[pin_id].cb != NULL) {
        s_irq[pin_id].cb(s_irq[pin_id].arg);
    }
}

static OPERATE_RET tkl_gpio_wl83_register(TUYA_GPIO_NUM_E pin_id, int edge)
{
    int index = exti_init((unsigned int)pin_id, (trigger_edge_t)edge,
                          tkl_gpio_wl83_dispatch, (void *)(uintptr_t)pin_id);
    if (index < 0) {
        /* exti_init refuses a pin with no external-interrupt capability. */
        return OPRT_NOT_SUPPORTED;
    }
    /* The index is documented as non-negative, so 0 is a valid registration and
     * cannot be used as the "not armed" sentinel - that would make the first
     * pin's registration invisible to disable/deinit and leave its callback
     * firing after the caller disabled it. exti_armed is the source of truth. */
    s_irq[pin_id].exti = index;
    s_irq[pin_id].armed = 1u;
    return OPRT_OK;
}

static void tkl_gpio_wl83_unregister(TUYA_GPIO_NUM_E pin_id)
{
    if (!s_irq[pin_id].armed) {
        return;
    }
    (void)exti_uninit(s_irq[pin_id].exti);
    s_irq[pin_id].armed = 0u;
    s_irq[pin_id].exti = -1;
}
#endif

OPERATE_RET tkl_gpio_init(TUYA_GPIO_NUM_E pin_id, const TUYA_GPIO_BASE_CFG_T *cfg)
{
    if (!tkl_gpio_valid(pin_id) || cfg == NULL) {
        return OPRT_INVALID_PARM;
    }

    if ((cfg->direct != TUYA_GPIO_OUTPUT && cfg->direct != TUYA_GPIO_INPUT) ||
        (cfg->mode != TUYA_GPIO_PULLUP && cfg->mode != TUYA_GPIO_PULLDOWN &&
         cfg->mode != TUYA_GPIO_HIGH_IMPEDANCE && cfg->mode != TUYA_GPIO_FLOATING &&
         cfg->mode != TUYA_GPIO_OPENDRAIN && cfg->mode != TUYA_GPIO_OPENDRAIN_PULLUP) ||
        (cfg->level != TUYA_GPIO_LEVEL_LOW && cfg->level != TUYA_GPIO_LEVEL_HIGH)) {
        return OPRT_INVALID_PARM;
    }

    /* Check the mode before touching the pin. The vendor API has no open-drain
     * setting - gpio_set_hd() selects a drive strength, not an output type - so
     * an open-drain request cannot be honoured. Driving it push-pull instead
     * would leave the line actively high where the caller expected it released,
     * which on a shared line is a conflict rather than a degradation. Refuse it
     * and leave the pin as it was. */
    if (cfg->direct == TUYA_GPIO_OUTPUT &&
        (cfg->mode == TUYA_GPIO_OPENDRAIN || cfg->mode == TUYA_GPIO_OPENDRAIN_PULLUP)) {
        return OPRT_NOT_SUPPORTED;
    }

    /* Digital function: the analogue alternative (die = 0) is only meaningful
     * for pins routed to the ADC, which this contract does not cover. */
    if (gpio_set_die((u32)pin_id, 1) != 0) return OPRT_COM_ERROR;

    if (cfg->direct == TUYA_GPIO_OUTPUT) {
        if (gpio_direction_output((u32)pin_id,
                                  cfg->level == TUYA_GPIO_LEVEL_HIGH ? 1 : 0) != 0) {
            return OPRT_COM_ERROR;
        }
        return tkl_gpio_apply_pull((u32)pin_id, cfg->mode) == 0 ? OPRT_OK : OPRT_COM_ERROR;
    }

    if (gpio_direction_input((u32)pin_id) != 0) return OPRT_COM_ERROR;

    /* Pull selection is mapped for both chips now: wl82 has a fixed 10K up/down
     * and wl83 picks the 10K tap. A mode with no pull (HIGH_IMPEDANCE,
     * FLOATING) clears both legs. */
    return tkl_gpio_apply_pull((u32)pin_id, cfg->mode) == 0 ? OPRT_OK : OPRT_COM_ERROR;
}

OPERATE_RET tkl_gpio_deinit(TUYA_GPIO_NUM_E pin_id)
{
    if (!tkl_gpio_valid(pin_id)) {
        return OPRT_INVALID_PARM;
    }
    /* Drop any interrupt first: leaving the vendor handler pointing at a pin
     * whose callback state is about to be cleared would call into stale state.
     * tkl_gpio_irq_disable is a no-op when nothing is registered. */
    (void)tkl_gpio_irq_disable(pin_id);
    s_irq[pin_id].cb = NULL;
    s_irq[pin_id].arg = NULL;

    /* Leave the pin as a floating input rather than holding whatever level it
     * was last driven to. */
    if (gpio_direction_input((u32)pin_id) != 0 ||
        tkl_gpio_apply_pull((u32)pin_id, TUYA_GPIO_FLOATING) != 0) {
        return OPRT_COM_ERROR;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_write(TUYA_GPIO_NUM_E pin_id, TUYA_GPIO_LEVEL_E level)
{
    if (!tkl_gpio_valid(pin_id)) {
        return OPRT_INVALID_PARM;
    }
    if (level != TUYA_GPIO_LEVEL_LOW && level != TUYA_GPIO_LEVEL_HIGH) {
        return OPRT_INVALID_PARM;
    }
    return gpio_direction_output((u32)pin_id, level == TUYA_GPIO_LEVEL_HIGH ? 1 : 0) == 0 ?
               OPRT_OK : OPRT_COM_ERROR;
}

OPERATE_RET tkl_gpio_read(TUYA_GPIO_NUM_E pin_id, TUYA_GPIO_LEVEL_E *level)
{
    if (!tkl_gpio_valid(pin_id) || level == NULL) {
        return OPRT_INVALID_PARM;
    }
    {
        int value = gpio_read((u32)pin_id);
        if (value < 0) return OPRT_COM_ERROR;
        *level = value ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_irq_init(TUYA_GPIO_NUM_E pin_id, const TUYA_GPIO_IRQ_T *cfg)
{
    int edge;

    if (!tkl_gpio_valid(pin_id) || cfg == NULL || cfg->cb == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (tkl_irq_edge(cfg->mode, &edge) != 0) {
        return OPRT_NOT_SUPPORTED;
    }

    /* Per the contract this arms nothing: it records what to register when the
     * caller enables the interrupt. Nothing is written to the vendor driver
     * here, which is also what keeps the "init does not enable" promise on
     * hardware where registering and enabling are the same operation. */
    if (s_irq[pin_id].armed) {
        /* Re-initialising an armed pin: hand the old registration back first,
         * or the vendor slot would leak and the new configuration would be
         * silently ignored until a disable. */
        (void)tkl_gpio_irq_disable(pin_id);
    }
    s_irq[pin_id].cb = cfg->cb;
    s_irq[pin_id].arg = cfg->arg;
    s_irq[pin_id].mode = cfg->mode;
    s_irq[pin_id].hdl = NULL;
    s_irq[pin_id].exti = -1;
    s_irq[pin_id].armed = 0u;
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_irq_enable(TUYA_GPIO_NUM_E pin_id)
{
    int edge;

    if (!tkl_gpio_valid(pin_id)) {
        return OPRT_INVALID_PARM;
    }
    if (s_irq[pin_id].cb == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (s_irq[pin_id].armed) {
        return OPRT_OK; /* already armed */
    }
    if (tkl_irq_edge(s_irq[pin_id].mode, &edge) != 0) {
        return OPRT_NOT_SUPPORTED;
    }

#if defined(JIELI_SELECTED_CHIP_WL82)
    return tkl_gpio_wl82_register(pin_id, edge);
#else
    return tkl_gpio_wl83_register(pin_id, edge);
#endif
}

OPERATE_RET tkl_gpio_irq_disable(TUYA_GPIO_NUM_E pin_id)
{
    if (!tkl_gpio_valid(pin_id)) {
        return OPRT_INVALID_PARM;
    }
    /* Both vendor drivers make "register" and "enable" the same act, so the
     * only way to stop the interrupt is to hand the registration back. A later
     * tkl_gpio_irq_enable re-registers from the saved configuration. */
#if defined(JIELI_SELECTED_CHIP_WL82)
    tkl_gpio_wl82_unregister(pin_id);
#else
    tkl_gpio_wl83_unregister(pin_id);
#endif
    return OPRT_OK;
}
