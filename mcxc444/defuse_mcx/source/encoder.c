/* Defuse to Wake - rotary encoder + SW3 on Port A. See encoder.h. */
#include "encoder.h"

#include "fsl_device_registers.h"

#include "app_config.h"
#include "input_queue.h"

#define ENC_A_MASK   (1u << APP_ENC_A_PIN)
#define ENC_B_MASK   (1u << APP_ENC_B_PIN)
#define SW3_MASK     (1u << APP_SW3_PIN)
#define STATE_BITS   2u      /* AB state = (A << 1) | B */
#define STATE_MASK   0x3u

/*
 * Quadrature transition table, indexed by (old_state << 2) | new_state.
 * +1 / -1 for the four valid Gray-code steps in each direction, 0 for "no change"
 * and for the impossible double transitions (both pins changed: a missed edge).
 * Contact bounce produces back-and-forth steps (+1 then -1) that cancel out.
 */
static const int8_t k_transition[16] = {
    0, -1, +1,  0,
   +1,  0,  0, -1,
   -1,  0,  0, +1,
    0, +1, -1,  0
};

static uint8_t s_state;          /* last AB state (ISR only) */
static int8_t s_accum;           /* valid transitions since the last detent (ISR only) */
static volatile int32_t s_count; /* detents since boot; 32-bit aligned reads are atomic */

static uint8_t read_ab(void)
{
    uint32_t pins = GPIOA->PDIR;
    uint8_t a = ((pins & ENC_A_MASK) != 0u) ? 1u : 0u;
    uint8_t b = ((pins & ENC_B_MASK) != 0u) ? 1u : 0u;
    return (uint8_t)((a << 1) | b);
}

static void config_input(uint32_t pin, uint32_t irqc)
{
    /* GPIO, pull-up enabled (PE = 1, PS = 1): the encoder/switch pulls the pin to GND. */
    PORTA->PCR[pin] = PORT_PCR_MUX(APP_GPIO_MUX) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK |
                      PORT_PCR_IRQC(irqc) | PORT_PCR_ISF_MASK;
    GPIOA->PDDR &= ~(1u << pin);
}

void encoder_init(void)
{
    NVIC_DisableIRQ(PORTA_IRQn);
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;

    config_input(APP_ENC_A_PIN, APP_IRQC_EITHER);
    config_input(APP_ENC_B_PIN, APP_IRQC_EITHER);
    config_input(APP_SW3_PIN, APP_IRQC_FALLING); /* re-mux PTA4 from NMI (reset ALT7) to GPIO */

    s_state = read_ab();
    s_accum = 0;
    s_count = 0;

    PORTA->ISFR = ENC_A_MASK | ENC_B_MASK | SW3_MASK; /* w1c: drop edges from start-up */
    /* 128: human-speed inputs, below UART2/TPM1 (64) whose bytes and timestamps would be
     * lost or skewed if delayed. FromISR calls are safe at any level on the CM0+ port. */
    NVIC_SetPriority(PORTA_IRQn, APP_NVIC_PRIO_GPIO);
    NVIC_ClearPendingIRQ(PORTA_IRQn);
    NVIC_EnableIRQ(PORTA_IRQn);
}

int32_t encoder_count(void)
{
    return s_count;
}

/* ISR: read pins, clear flags, decode, post. No blocking, no loops on input. */
void PORTA_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;
    uint32_t flags = PORTA->ISFR;
    uint8_t now;
    int8_t step;

    /* Clear exactly the flags we are about to handle (w1c). Writing back only these
     * bits never discards an edge that arrives after the read. */
    PORTA->ISFR = flags & (ENC_A_MASK | ENC_B_MASK | SW3_MASK);

    if ((flags & (ENC_A_MASK | ENC_B_MASK)) != 0u) {
        now = read_ab();
        step = k_transition[((s_state << STATE_BITS) | now) & 0xFu];
        s_state = (uint8_t)(now & STATE_MASK);
        s_accum = (int8_t)(s_accum + step);
        if (s_accum >= APP_ENC_TRANSITIONS_PER_STEP || s_accum <= -APP_ENC_TRANSITIONS_PER_STEP) {
            int32_t dir = (s_accum > 0) ? 1 : -1;
#if APP_ENC_INVERT
            dir = -dir;
#endif
            s_accum = 0;
            s_count += dir;
            input_post_from_isr(GAME_EVT_ENC_STEP, dir, &woken);
        }
    }

    if ((flags & SW3_MASK) != 0u) {
        input_post_from_isr(GAME_EVT_CMD_RING_NOW, APP_SW3_RING_DELAY_S, &woken);
    }

    portYIELD_FROM_ISR(woken);
}
