/* Defuse to Wake - HC-SR04 on TPM1 input capture. See ultrasonic.h. */
#include "ultrasonic.h"

#include "fsl_device_registers.h"

#include "app_config.h"
#include "input_queue.h"

#define TRIG_MASK      (1u << APP_US_TRIG_PIN)
#define ECHO_MASK      (1u << APP_US_ECHO_PIN)
#define WIDTH_INVALID  0xFFFFFFFFu  /* s_width_us marker: this cycle's echo was not usable */
#define TPM_CMOD_COUNT 1u   /* SC CMOD: count on every TPM counter clock */
#define MAX_OVERFLOWS  1u   /* > 1 wrap = pulse > 65 ms: impossible, discard */

typedef enum { ECHO_IDLE = 0, ECHO_WAIT_RISE, ECHO_WAIT_FALL } echo_state_t;

static TaskHandle_t s_task;
static volatile echo_state_t s_state;   /* ISR + task (task only writes while idle) */
static volatile uint16_t s_rise;        /* ISR only */
static volatile uint32_t s_overflows;   /* ISR only */
static volatile uint32_t s_width_us;    /* ISR -> task, read after the notification */
static volatile int32_t s_last_cm = APP_US_INVALID_CM;
static volatile uint32_t s_timeouts;
static volatile uint32_t s_stale;      /* cycles discarded by the capture rule */

/* Bounded wait for a CnSC write to be acknowledged in the TPM clock domain (RM TPM
 * chapter: a channel must be disabled, and that acknowledged, before a mode change).
 * Init-time only. */
static void tpm1_write_cnsc(uint32_t value)
{
    uint32_t polls = 0;

    TPM1->CONTROLS[APP_US_ECHO_CH].CnSC = 0u;
    while (TPM1->CONTROLS[APP_US_ECHO_CH].CnSC != 0u && polls < APP_TPM_ACK_POLLS) {
        polls++;
    }
    TPM1->CONTROLS[APP_US_ECHO_CH].CnSC = value;
    polls = 0;
    while ((TPM1->CONTROLS[APP_US_ECHO_CH].CnSC & ~TPM_CnSC_CHF_MASK) != value && polls < APP_TPM_ACK_POLLS) {
        polls++;
    }
}

void ultrasonic_init(void)
{
    NVIC_DisableIRQ(TPM1_IRQn);
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK | SIM_SCGC5_PORTB_MASK;
    SIM->SCGC6 |= SIM_SCGC6_TPM1_MASK;

    /* TPM counter clock = MCGIRCLK. BOARD_BootClockRUN already enables the 8 MHz LIRC
     * as MCGIRCLK (IRCLKEN, LIRC_DIV2 = /1), so the MCG is left untouched. */
    SIM->SOPT2 = (SIM->SOPT2 & ~SIM_SOPT2_TPMSRC_MASK) | SIM_SOPT2_TPMSRC(APP_TPMSRC_MCGIRCLK);

    /* TRIG: GPIO output, idle low. */
    PORTB->PCR[APP_US_TRIG_PIN] = PORT_PCR_MUX(APP_GPIO_MUX);
    GPIOB->PCOR = TRIG_MASK;
    GPIOB->PDDR |= TRIG_MASK;

    /* ECHO: PTA13 ALT3 = TPM1_CH1 (through the 5 V -> 3.3 V divider). */
    PORTA->PCR[APP_US_ECHO_PIN] = PORT_PCR_MUX(APP_TPM_ALT3_MUX);

    /* TPM1: stopped while configuring; 8 MHz / 8 = 1 MHz -> 1 us per count; free-running
     * 16-bit up-counter (MOD = 0xFFFF, edge-aligned, CPWMS = 0). */
    TPM1->SC = 0u;
    TPM1->CNT = 0u; /* any write clears the counter */
    TPM1->MOD = APP_TPM1_MOD;
    /* Input capture on both edges: MSB:MSA = 00, ELSB:ELSA = 11, interrupt on capture. */
    tpm1_write_cnsc(TPM_CnSC_ELSB_MASK | TPM_CnSC_ELSA_MASK | TPM_CnSC_CHIE_MASK);
    TPM1->STATUS = TPM_STATUS_CH1F_MASK | TPM_STATUS_TOF_MASK; /* w1c */
    TPM1->SC = TPM_SC_TOIE_MASK | TPM_SC_CMOD(TPM_CMOD_COUNT) | TPM_SC_PS(APP_TPM_PS_DIV8);

    s_state = ECHO_IDLE;
    /* 64: the edge timestamps are latched by hardware, but the rising edge must be
     * handled before the falling one; same urgency as UART2, above GPIO/ADC (128). */
    NVIC_SetPriority(TPM1_IRQn, APP_NVIC_PRIO_TPM1);
    NVIC_ClearPendingIRQ(TPM1_IRQn);
    NVIC_EnableIRQ(TPM1_IRQn);
}

/*
 * ECHO CAPTURE RULE (re-armed at every ping):
 *   1. A ping starts only if ECHO is LOW. If it is still HIGH, the previous echo has
 *      not ended: nothing is triggered and the cycle reports DISTANCE 999.
 *   2. Before the trigger the capture flag is cleared and the state set to WAIT_RISE,
 *      so only edges captured AFTER this trigger can count. Captures that arrive while
 *      the state is IDLE (e.g. the late falling edge of a ping we already gave up on)
 *      are discarded.
 *   3. The first accepted edge must be a RISING edge (pin high when the ISR runs), the
 *      next a FALLING edge (pin low). Any other order (a stray falling edge first, or
 *      a second rising edge) invalidates the cycle: it reports 999.
 *   4. If either edge never comes, the 35 ms task timeout also reports 999.
 * The pin level is read through GPIOA->PDIR (a pin muxed to a TPM channel is still a
 * digital function and reads its level, RM GPIO "PDIR" p.842).
 * UNVERIFIED on hardware: set APP_US_USE_PIN_LEVEL 0 to fall back to edge ORDER only.
 */

/* ISR: read the captured value, clear flags, classify the edge, notify. */
void TPM1_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;
    uint32_t status = TPM1->STATUS;
    bool level_high;
    uint16_t cap;

    TPM1->STATUS = status & (TPM_STATUS_CH1F_MASK | TPM_STATUS_TOF_MASK); /* w1c */

    if ((status & TPM_STATUS_TOF_MASK) != 0u && s_state == ECHO_WAIT_FALL) {
        s_overflows++;
    }
    if ((status & TPM_STATUS_CH1F_MASK) != 0u) {
        cap = (uint16_t)TPM1->CONTROLS[APP_US_ECHO_CH].CnV;
        level_high = (GPIOA->PDIR & ECHO_MASK) != 0u;
        if (s_state == ECHO_WAIT_RISE) {
            if (!APP_US_USE_PIN_LEVEL || level_high) {
                s_rise = cap;
                s_overflows = 0u;
                s_state = ECHO_WAIT_FALL;
            } else { /* a falling edge first: left over from an older echo */
                s_width_us = WIDTH_INVALID;
                s_stale++;
                s_state = ECHO_IDLE;
                if (s_task != NULL) { vTaskNotifyGiveFromISR(s_task, &woken); }
            }
        } else if (s_state == ECHO_WAIT_FALL) {
            if (!APP_US_USE_PIN_LEVEL || !level_high) {
                /* uint16_t subtraction handles one counter wrap by itself; the overflow
                 * count only rejects pulses longer than 65.5 ms (not an HC-SR04 echo). */
                s_width_us = (s_overflows > MAX_OVERFLOWS) ? WIDTH_INVALID
                                                           : (uint32_t)(uint16_t)(cap - s_rise);
            } else { /* a second rising edge: not a valid pulse */
                s_width_us = WIDTH_INVALID;
                s_stale++;
            }
            s_state = ECHO_IDLE;
            if (s_task != NULL) { vTaskNotifyGiveFromISR(s_task, &woken); }
        } else {
            /* ECHO_IDLE: a capture nobody is waiting for: discarded. */
        }
    }
    portYIELD_FROM_ISR(woken);
}

/* 10 us TRIG pulse. The ONLY busy-wait in the firmware: it is far shorter than one
 * RTOS tick (5 ms) and is timed by the TPM1 1 MHz counter itself, so it does not
 * depend on the CPU clock or compiler optimisation. A critical section keeps an ISR
 * from stretching the pulse. */
static void trigger_pulse(void)
{
    uint16_t start;
    uint32_t guard = 0;

    taskENTER_CRITICAL();
    GPIOB->PSOR = TRIG_MASK;
    start = (uint16_t)TPM1->CNT;
    while ((uint16_t)((uint16_t)TPM1->CNT - start) < APP_US_TRIG_US && guard < APP_TPM_ACK_POLLS) {
        guard++;
    }
    GPIOB->PCOR = TRIG_MASK;
    taskEXIT_CRITICAL();
}

static void ultrasonic_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    int32_t cm;

    (void)arg;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_US_PERIOD_MS));

        /* Rule 1: never ping while the previous echo is still high. */
        if (APP_US_USE_PIN_LEVEL && (GPIOA->PDIR & ECHO_MASK) != 0u) {
            s_stale++;
            s_last_cm = APP_US_INVALID_CM;
            input_post(GAME_EVT_DISTANCE, APP_US_INVALID_CM);
            continue;
        }

        /* Rule 2: re-arm. Drop a late notification and any pending capture flag, then
         * arm WAIT_RISE before the pulse so only edges after this trigger count. */
        (void)ulTaskNotifyTake(pdTRUE, 0);
        TPM1->STATUS = TPM_STATUS_CH1F_MASK; /* w1c */
        s_width_us = WIDTH_INVALID;
        s_state = ECHO_WAIT_RISE;
        trigger_pulse();

        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(APP_US_ECHO_TIMEOUT_MS)) == 0u) {
            s_state = ECHO_IDLE;   /* rule 4: late edges are now discarded by the ISR */
            s_timeouts++;
            cm = APP_US_INVALID_CM;
        } else if (s_width_us == WIDTH_INVALID) {
            cm = APP_US_INVALID_CM; /* rule 3: wrong edge order */
        } else {
            cm = (int32_t)(s_width_us / APP_US_US_PER_CM);
            if (cm > (int32_t)APP_US_MAX_CM) {
                cm = APP_US_INVALID_CM;
            }
        }
        s_last_cm = cm;
        input_post(GAME_EVT_DISTANCE, cm);
    }
}

bool ultrasonic_start(void)
{
    return xTaskCreate(ultrasonic_task, "ultrasonic", APP_STACK_ULTRASONIC, NULL, APP_PRIO_ULTRASONIC, &s_task) == pdPASS;
}

int32_t ultrasonic_last_cm(void)
{
    return s_last_cm;
}

uint32_t ultrasonic_timeouts(void)
{
    return s_timeouts;
}

uint32_t ultrasonic_stale(void)
{
    return s_stale;
}

TaskHandle_t ultrasonic_task_handle(void)
{
    return s_task;
}
