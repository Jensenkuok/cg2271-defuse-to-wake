/* Defuse to Wake - buzzer + RGB LED feedback task. See feedback.h. */
#include "feedback.h"

#include "fsl_device_registers.h"
#include "queue.h"

#include "app_config.h"

#define TPM_CMOD_COUNT  1u   /* SC CMOD: count on every TPM counter clock */
#define MAX_NOTES       4u
#define NOTE_SILENT     0u   /* freq 0 = rest */

typedef enum { REQ_PLAY = 0, REQ_TICK, REQ_LED, REQ_LINK } req_kind_t;

typedef struct {
    uint8_t kind;   /* req_kind_t */
    uint16_t value;
} feedback_req_t;

typedef struct {
    uint16_t freq_hz;
    uint16_t dur_ms;   /* 0 = end of pattern */
} note_t;

/* One pattern per game_sound_t (same order as the enum), MAX_NOTES + terminator. */
static const note_t k_sounds[][MAX_NOTES + 1u] = {
    [GAME_SOUND_CLICK]          = { {3000, 15}, {0, 0} },
    [GAME_SOUND_DIGIT_OK]       = { {1500, 60}, {2000, 60}, {0, 0} },
    [GAME_SOUND_STRIKE]         = { {200, 350}, {0, 0} },
    [GAME_SOUND_MODULE_CLEARED] = { {1000, 80}, {1500, 80}, {2000, 140}, {0, 0} },
    [GAME_SOUND_DEFUSED]        = { {523, 150}, {659, 150}, {784, 150}, {1047, 450}, {0, 0} },
    [GAME_SOUND_EXPLODED]       = { {150, 250}, {120, 250}, {100, 700}, {0, 0} },
    [GAME_SOUND_DENIED]         = { {400, 100}, {NOTE_SILENT, 50}, {400, 100}, {0, 0} },
};
#define SOUND_COUNT (sizeof(k_sounds) / sizeof(k_sounds[0]))

static TaskHandle_t s_task;
static QueueHandle_t s_queue;
static volatile uint32_t s_drops;

/* ---- task-owned state ---- */
static const note_t *s_pattern;   /* NULL = no sound playing */
static uint32_t s_note_end_ms;
static uint16_t s_tick_ms;        /* 0 = no ticking */
static uint32_t s_next_tick_ms;
static bool s_click_on;
static uint32_t s_click_end_ms;
static uint16_t s_buzzer_hz;      /* frequency currently on TPM2 (0 = off) */
static game_led_t s_led_mode = GAME_LED_OFF;
static bool s_link_up;

static bool reached(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0; /* wraparound-safe */
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* Bounded CnSC write acknowledge (init-time only), see ultrasonic.c. */
static void tpm_write_cnsc(TPM_Type *tpm, uint32_t ch, uint32_t value)
{
    uint32_t polls = 0;

    tpm->CONTROLS[ch].CnSC = 0u;
    while (tpm->CONTROLS[ch].CnSC != 0u && polls < APP_TPM_ACK_POLLS) {
        polls++;
    }
    tpm->CONTROLS[ch].CnSC = value;
    polls = 0;
    while ((tpm->CONTROLS[ch].CnSC & ~TPM_CnSC_CHF_MASK) != value && polls < APP_TPM_ACK_POLLS) {
        polls++;
    }
}

static void tpm0_led_init(void)
{
    SIM->SCGC6 |= SIM_SCGC6_TPM0_MASK;
    SIM->SCGC5 |= SIM_SCGC5_PORTD_MASK | SIM_SCGC5_PORTE_MASK;
    PORTE->PCR[APP_LED_RED_PIN] = PORT_PCR_MUX(APP_TPM_ALT3_MUX);
    PORTD->PCR[APP_LED_GREEN_PIN] = PORT_PCR_MUX(APP_TPM_ALT4_MUX);
    PORTE->PCR[APP_LED_BLUE_PIN] = PORT_PCR_MUX(APP_TPM_ALT3_MUX);

    /* Edge-aligned PWM, 1 MHz / (MOD + 1) = 1 kHz. MSB:ELSA = low-true pulses: the pin
     * is low (LED on, it is active-low) for CnV counts, so CnV = duty of "on". */
    TPM0->SC = 0u;
    TPM0->CNT = 0u;
    TPM0->MOD = APP_LED_PWM_MOD;
    tpm_write_cnsc(TPM0, APP_LED_RED_CH, TPM_CnSC_MSB_MASK | TPM_CnSC_ELSA_MASK);
    tpm_write_cnsc(TPM0, APP_LED_GREEN_CH, TPM_CnSC_MSB_MASK | TPM_CnSC_ELSA_MASK);
    tpm_write_cnsc(TPM0, APP_LED_BLUE_CH, TPM_CnSC_MSB_MASK | TPM_CnSC_ELSA_MASK);
    TPM0->CONTROLS[APP_LED_RED_CH].CnV = 0u;
    TPM0->CONTROLS[APP_LED_GREEN_CH].CnV = 0u;
    TPM0->CONTROLS[APP_LED_BLUE_CH].CnV = 0u;
    TPM0->SC = TPM_SC_CMOD(TPM_CMOD_COUNT) | TPM_SC_PS(APP_TPM_PS_DIV8);
}

static void tpm2_buzzer_init(void)
{
    SIM->SCGC6 |= SIM_SCGC6_TPM2_MASK;
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;
    PORTB->PCR[APP_BUZZER_PIN] = PORT_PCR_MUX(APP_TPM_ALT3_MUX);

    /* Edge-aligned, high-true PWM (MSB:ELSB). Tone = 1 MHz / (MOD + 1); 50 % duty while
     * sounding, CnV = 0 (pin held low) when silent. */
    TPM2->SC = 0u;
    TPM2->CNT = 0u;
    TPM2->MOD = (APP_TPM_TICK_HZ / APP_TICK_CLICK_HZ) - 1u;
    tpm_write_cnsc(TPM2, APP_BUZZER_CH, TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK);
    TPM2->CONTROLS[APP_BUZZER_CH].CnV = 0u;
    TPM2->SC = TPM_SC_CMOD(TPM_CMOD_COUNT) | TPM_SC_PS(APP_TPM_PS_DIV8);
}

static void buzzer_set(uint16_t hz)
{
    uint32_t mod;

    if (hz == s_buzzer_hz) {
        return;
    }
    s_buzzer_hz = hz;
    if (hz == NOTE_SILENT) {
        TPM2->CONTROLS[APP_BUZZER_CH].CnV = 0u;
        return;
    }
    mod = (APP_TPM_TICK_HZ / hz) - 1u;
    TPM2->MOD = mod;
    TPM2->CNT = 0u; /* restart the period so a shorter MOD takes effect at once */
    TPM2->CONTROLS[APP_BUZZER_CH].CnV = (mod + 1u) / 2u;
}

static void led_pwm(uint32_t r_pct, uint32_t g_pct, uint32_t b_pct)
{
    uint32_t full = APP_LED_PWM_MOD + 1u;

    TPM0->CONTROLS[APP_LED_RED_CH].CnV = r_pct * full / APP_PERCENT;
    TPM0->CONTROLS[APP_LED_GREEN_CH].CnV = g_pct * full / APP_PERCENT;
    TPM0->CONTROLS[APP_LED_BLUE_CH].CnV = b_pct * full / APP_PERCENT;
}

static void update_led(uint32_t now)
{
    uint32_t on = APP_LED_FULL_PCT;

    switch (s_led_mode) {
    case GAME_LED_ARMED_DIM_BLUE:
        led_pwm(0u, 0u, APP_LED_DIM_PCT);
        break;
    case GAME_LED_ACTIVE_RED_BLINK:
        led_pwm(((now / APP_LED_ACTIVE_BLINK_MS) % 2u == 0u) ? on : 0u, 0u, 0u);
        break;
    case GAME_LED_DEFUSED_GREEN:
        led_pwm(0u, on, 0u);
        break;
    case GAME_LED_EXPLODED_RED_WHITE:
        if ((now / APP_LED_EXPLODED_SWAP_MS) % 2u == 0u) {
            led_pwm(on, 0u, 0u);
        } else {
            led_pwm(on, on, on);
        }
        break;
    default: /* OFF: only the link-down heartbeat blink */
        led_pwm(0u, 0u, (!s_link_up && (now % APP_LED_LINKDOWN_PERIOD_MS) < APP_LED_LINKDOWN_ON_MS) ? on : 0u);
        break;
    }
}

static void update_buzzer(uint32_t now)
{
    if (s_pattern != NULL) {
        if (reached(now, s_note_end_ms)) {
            s_pattern++;
            if (s_pattern->dur_ms == 0u) {
                s_pattern = NULL; /* pattern finished */
            } else {
                s_note_end_ms = now + s_pattern->dur_ms;
            }
        }
        buzzer_set((s_pattern != NULL) ? s_pattern->freq_hz : NOTE_SILENT);
        if (s_pattern != NULL) {
            return; /* a sound pre-empts the ticking */
        }
    }
    if (s_click_on && reached(now, s_click_end_ms)) {
        s_click_on = false;
    }
    if (s_tick_ms != 0u && reached(now, s_next_tick_ms)) {
        s_click_on = true;
        s_click_end_ms = now + APP_TICK_CLICK_MS;
        s_next_tick_ms = now + s_tick_ms;
    }
    buzzer_set(s_click_on ? (uint16_t)APP_TICK_CLICK_HZ : NOTE_SILENT);
}

static void handle(const feedback_req_t *r, uint32_t now)
{
    switch (r->kind) {
    case REQ_PLAY:
        if (r->value < SOUND_COUNT) {
            s_pattern = k_sounds[r->value];
            s_note_end_ms = now + s_pattern->dur_ms;
        }
        break;
    case REQ_TICK:
        s_tick_ms = r->value;
        s_next_tick_ms = now;
        break;
    case REQ_LED:
        s_led_mode = (game_led_t)r->value;
        break;
    default:
        s_link_up = (r->value != 0u);
        break;
    }
}

static void feedback_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    feedback_req_t r;
    uint32_t now;

    (void)arg;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_FEEDBACK_PERIOD_MS));
        now = now_ms();
        while (xQueueReceive(s_queue, &r, 0) == pdTRUE) {
            handle(&r, now);
        }
        update_buzzer(now);
        update_led(now);
    }
}

static void post(req_kind_t kind, uint16_t value)
{
    feedback_req_t r;

    r.kind = (uint8_t)kind;
    r.value = value;
    if (s_queue == NULL || xQueueSend(s_queue, &r, 0) != pdTRUE) {
        taskENTER_CRITICAL();
        s_drops++;
        taskEXIT_CRITICAL();
    }
}

bool feedback_init(void)
{
    s_queue = xQueueCreate(APP_FEEDBACK_QUEUE_LEN, sizeof(feedback_req_t));
    if (s_queue == NULL) {
        return false;
    }
    /* Shared TPM counter clock: MCGIRCLK (8 MHz, set up by BOARD_BootClockRUN). */
    SIM->SOPT2 = (SIM->SOPT2 & ~SIM_SOPT2_TPMSRC_MASK) | SIM_SOPT2_TPMSRC(APP_TPMSRC_MCGIRCLK);
    tpm0_led_init();
    tpm2_buzzer_init();
    return true;
}

bool feedback_start(void)
{
    return xTaskCreate(feedback_task, "feedback", APP_STACK_FEEDBACK, NULL, APP_PRIO_FEEDBACK, &s_task) == pdPASS;
}

void feedback_play(game_sound_t s)               { post(REQ_PLAY, (uint16_t)s); }
void feedback_set_tick_interval(uint16_t ms)     { post(REQ_TICK, ms); }
void feedback_set_led(game_led_t mode)           { post(REQ_LED, (uint16_t)mode); }
void feedback_set_link(bool up)                  { post(REQ_LINK, up ? 1u : 0u); }
uint32_t feedback_drops(void)                    { return s_drops; }
TaskHandle_t feedback_task_handle(void)          { return s_task; }
