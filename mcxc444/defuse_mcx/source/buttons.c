/* Defuse to Wake - push buttons on PORTC/PORTD. See buttons.h. */
#include "buttons.h"

#include <stdbool.h>
#include <stdint.h>

#include "fsl_device_registers.h"

#include "FreeRTOS.h"
#include "task.h"

#include "app_config.h"
#include "input_queue.h"

#define ENC_SW_MASK (1u << APP_ENC_SW_PIN)
#define JOY_SW_MASK (1u << APP_JOY_SW_PIN)
#define SHOCK_MASK  (1u << APP_SHOCK_PIN)

typedef struct {
    bool pressed;           /* last accepted level (pressed = pin low) */
    bool seen;              /* at least one edge accepted (debounce reference valid) */
    TickType_t last_edge;   /* tick of the last accepted edge */
    TickType_t press_tick;  /* tick of the last accepted press */
} button_t;

static button_t s_enc_sw;
static button_t s_joy_sw;
#if APP_USE_SHOCK_FALLBACK
static button_t s_shock;
#endif

static void config_input(PORT_Type *port, GPIO_Type *gpio, uint32_t pin, uint32_t irqc)
{
    /* GPIO, pull-up: the switch connects the pin to GND (pressed = low). */
    port->PCR[pin] = PORT_PCR_MUX(APP_GPIO_MUX) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK |
                     PORT_PCR_IRQC(irqc) | PORT_PCR_ISF_MASK;
    gpio->PDDR &= ~(1u << pin);
}

void buttons_init(void)
{
    NVIC_DisableIRQ(PORTC_PORTD_IRQn);
    SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK | SIM_SCGC5_PORTD_MASK;

    config_input(PORTC, GPIOC, APP_ENC_SW_PIN, APP_IRQC_EITHER);
    config_input(PORTC, GPIOC, APP_JOY_SW_PIN, APP_IRQC_EITHER);
    PORTC->ISFR = ENC_SW_MASK | JOY_SW_MASK;
#if APP_USE_SHOCK_FALLBACK
    config_input(PORTD, GPIOD, APP_SHOCK_PIN, APP_IRQC_FALLING);
    PORTD->ISFR = SHOCK_MASK;
#endif

    /* 128: same level as Port A; buttons are human-speed and must not delay the
     * UART2/TPM1 ISRs at 64. Port C and Port D share this one vector. */
    NVIC_SetPriority(PORTC_PORTD_IRQn, APP_NVIC_PRIO_GPIO);
    NVIC_ClearPendingIRQ(PORTC_PORTD_IRQn);
    NVIC_EnableIRQ(PORTC_PORTD_IRQn);
}

/* Accept an edge if the level really changed and the debounce window has passed. */
static bool accept_edge(button_t *b, bool pressed_now, TickType_t now)
{
    if (pressed_now == b->pressed) {
        return false; /* bounce back to the level we already have */
    }
    if (b->seen && (TickType_t)(now - b->last_edge) < pdMS_TO_TICKS(APP_BTN_DEBOUNCE_MS)) {
        return false;
    }
    b->seen = true;
    b->last_edge = now;
    b->pressed = pressed_now;
    return true;
}

/* ISR: read pins, clear flags, decide, post. No blocking, no loops on input. */
void PORTC_PORTD_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;
    TickType_t now = xTaskGetTickCountFromISR();
    uint32_t cflags = PORTC->ISFR;
    uint32_t cpins;

    PORTC->ISFR = cflags & (ENC_SW_MASK | JOY_SW_MASK); /* w1c, only what we handle */
    cpins = GPIOC->PDIR;

    if ((cflags & ENC_SW_MASK) != 0u &&
        accept_edge(&s_enc_sw, (cpins & ENC_SW_MASK) == 0u, now)) {
        if (s_enc_sw.pressed) {
            s_enc_sw.press_tick = now;
        } else if ((TickType_t)(now - s_enc_sw.press_tick) < pdMS_TO_TICKS(APP_LONG_PRESS_MS)) {
            input_post_from_isr(GAME_EVT_ENC_PRESS, 0, &woken);
        } else {
            input_post_from_isr(GAME_EVT_ENC_LONG_PRESS, 0, &woken);
        }
    }

    if ((cflags & JOY_SW_MASK) != 0u &&
        accept_edge(&s_joy_sw, (cpins & JOY_SW_MASK) == 0u, now) && s_joy_sw.pressed) {
        input_post_from_isr(GAME_EVT_JOY_PRESS, 0, &woken);
    }

#if APP_USE_SHOCK_FALLBACK
    {
        uint32_t dflags = PORTD->ISFR;
        PORTD->ISFR = dflags & SHOCK_MASK;
        /* Falling edge only: treat every accepted hit as a press-and-release. */
        if ((dflags & SHOCK_MASK) != 0u && accept_edge(&s_shock, true, now)) {
            s_shock.pressed = false; /* re-arm for the next hit (still debounced) */
            input_post_from_isr(GAME_EVT_TAMPER, APP_SHOCK_TAMPER_MG, &woken);
        }
    }
#endif

    portYIELD_FROM_ISR(woken);
}
