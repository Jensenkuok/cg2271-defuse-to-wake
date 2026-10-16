/* Defuse to Wake - joystick sampler on ADC0. See joystick.h. */
#include "joystick.h"

#include "fsl_device_registers.h"

#include "app_config.h"
#include "input_queue.h"

#define ADC_DIV_8           3u   /* CFG1 ADIV: bus 24 MHz / 8 = 3 MHz ADCK (RM: <= 4 MHz for CAL) */
#define ADC_CLK_BUS         0u   /* CFG1 ADICLK: bus clock */
#define ADC_MODE_12BIT      1u   /* CFG1 MODE: 12-bit single-ended */
#define ADC_REF_VALT        1u   /* SC2 REFSEL: alternate pair = VDDA (RM Table 2-13); VREFH
                                  * is not wired to 3.3 V by default (UM12120 JP6 open) */
#define ADC_AVG_32          3u   /* SC3 AVGS: 32 samples, as the RM asks for calibration */
#define ADC_AVG_4           0u   /* SC3 AVGS: 4 samples for normal conversions (noise) */

typedef enum { JOY_ARMED = 0, JOY_FIRED } joy_arm_t;

static TaskHandle_t s_task;
static volatile uint16_t s_result;      /* written by the ADC ISR */
static volatile uint16_t s_raw_x;
static volatile uint16_t s_raw_y;
static volatile uint32_t s_errors;

/* One-time calibration per RM ADC "Calibration function" (pp.391-392). Runs before the scheduler, so the bounded poll of
 * the CAL bit here is init-time only (no task busy-waits). */
static bool adc_calibrate(void)
{
    uint32_t polls = 0;
    uint32_t sum;

    ADC0->SC3 = ADC_SC3_AVGE_MASK | ADC_SC3_AVGS(ADC_AVG_32) | ADC_SC3_CAL_MASK;
    while ((ADC0->SC3 & ADC_SC3_CAL_MASK) != 0u && polls < APP_ADC_CAL_MAX_POLLS) {
        polls++;
    }
    if ((ADC0->SC3 & (ADC_SC3_CAL_MASK | ADC_SC3_CALF_MASK)) != 0u) {
        return false; /* CALF set or never finished */
    }
    /* Plus side: sum CLP0..4 + CLPS, halve, set MSB, store in PG. Minus side into MG. */
    sum = ADC0->CLP0 + ADC0->CLP1 + ADC0->CLP2 + ADC0->CLP3 + ADC0->CLP4 + ADC0->CLPS;
    ADC0->PG = (sum / 2u) | APP_ADC_GAIN_MSB;
    sum = ADC0->CLM0 + ADC0->CLM1 + ADC0->CLM2 + ADC0->CLM3 + ADC0->CLM4 + ADC0->CLMS;
    ADC0->MG = (sum / 2u) | APP_ADC_GAIN_MSB;
    (void)ADC0->R[0]; /* calibration end sets COCO; reading R clears it */
    return true;
}

bool joystick_init(void)
{
    bool ok;

    NVIC_DisableIRQ(ADC0_IRQn);
    SIM->SCGC6 |= SIM_SCGC6_ADC0_MASK;
    SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;
    PORTE->PCR[APP_JOY_X_PIN] = PORT_PCR_MUX(APP_ANALOG_MUX);
    PORTE->PCR[APP_JOY_Y_PIN] = PORT_PCR_MUX(APP_ANALOG_MUX);

    /* Long sample time: the joystick pots are high-impedance sources. */
    ADC0->CFG1 = ADC_CFG1_ADIV(ADC_DIV_8) | ADC_CFG1_ADLSMP_MASK |
                 ADC_CFG1_MODE(ADC_MODE_12BIT) | ADC_CFG1_ADICLK(ADC_CLK_BUS);
    ADC0->CFG2 = 0u;                              /* MUXSEL = 0: 'a' channels (SE4a) */
    ADC0->SC2 = ADC_SC2_REFSEL(ADC_REF_VALT);     /* ADTRG = 0: software trigger */
    ok = adc_calibrate();
    if (!ok) {
        s_errors++;
    }
    ADC0->SC3 = ADC_SC3_AVGE_MASK | ADC_SC3_AVGS(ADC_AVG_4); /* single conversion, 4x avg */

    /* 128: a conversion result for a 50 Hz sample is not urgent; stays below the
     * timing-critical UART2/TPM1 ISRs (64). */
    NVIC_SetPriority(ADC0_IRQn, APP_NVIC_PRIO_ADC0);
    NVIC_ClearPendingIRQ(ADC0_IRQn);
    NVIC_EnableIRQ(ADC0_IRQn);
    return ok;
}

/* ISR: read the result (clears COCO), wake the task. */
void ADC0_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;

    if ((ADC0->SC1[0] & ADC_SC1_COCO_MASK) != 0u) {
        s_result = (uint16_t)ADC0->R[0];
        if (s_task != NULL) {
            vTaskNotifyGiveFromISR(s_task, &woken);
        }
    }
    portYIELD_FROM_ISR(woken);
}

/* Start one conversion and block (not spin) until the ISR notifies us. */
static bool adc_read(uint32_t channel, uint16_t *out)
{
    (void)ulTaskNotifyTake(pdTRUE, 0); /* drop any stale notification */
    ADC0->SC1[0] = ADC_SC1_AIEN_MASK | ADC_SC1_ADCH(channel); /* write = software trigger */
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(APP_JOY_ADC_TIMEOUT_MS)) == 0u) {
        s_errors++;
        return false;
    }
    *out = s_result;
    return true;
}

/* Edge events with hysteresis: fire once on crossing LOW/HIGH, re-arm in the centre. */
static void x_to_events(uint16_t x, joy_arm_t *arm)
{
    game_event_type_t low_evt = APP_JOY_X_INVERT ? GAME_EVT_JOY_RIGHT : GAME_EVT_JOY_LEFT;
    game_event_type_t high_evt = APP_JOY_X_INVERT ? GAME_EVT_JOY_LEFT : GAME_EVT_JOY_RIGHT;

    if (*arm == JOY_ARMED) {
        if (x < APP_JOY_LOW) {
            input_post(low_evt, x);
            *arm = JOY_FIRED;
        } else if (x > APP_JOY_HIGH) {
            input_post(high_evt, x);
            *arm = JOY_FIRED;
        }
    } else if (x >= APP_JOY_CENTER_LO && x <= APP_JOY_CENTER_HI) {
        *arm = JOY_ARMED;
    }
}

static void joystick_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    joy_arm_t arm = JOY_ARMED;
    uint16_t x;
    uint16_t y;

    (void)arg;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_JOY_PERIOD_MS));
        if (!adc_read(APP_JOY_X_ADC_CH, &x) || !adc_read(APP_JOY_Y_ADC_CH, &y)) {
            continue; /* skip this sample; counted in s_errors */
        }
        taskENTER_CRITICAL(); /* keep the X/Y pair consistent for readers */
        s_raw_x = x;
        s_raw_y = y;
        taskEXIT_CRITICAL();
        x_to_events(x, &arm);
    }
}

bool joystick_start(void)
{
    return xTaskCreate(joystick_task, "joystick", APP_STACK_JOYSTICK, NULL, APP_PRIO_JOYSTICK, &s_task) == pdPASS;
}

void joystick_get_raw(uint16_t *x, uint16_t *y)
{
    taskENTER_CRITICAL();
    *x = s_raw_x;
    *y = s_raw_y;
    taskEXIT_CRITICAL();
}

uint32_t joystick_errors(void)
{
    return s_errors;
}

TaskHandle_t joystick_task_handle(void)
{
    return s_task;
}
