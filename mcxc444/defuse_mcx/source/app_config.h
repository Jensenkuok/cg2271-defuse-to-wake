/*
 * Defuse to Wake - MCXC444 application configuration.
 * Every pin, priority, period, threshold, stack size and queue length lives here
 * (no magic numbers in the .c files). Pin sources: docs/pin_table.md.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "FreeRTOS.h"

/* ---- Build options ---- */
#ifndef APP_LOOPBACK_TEST
#define APP_LOOPBACK_TEST           0   /* 1 = UART2 self-test, jumper PTE22<->PTE23 */
#endif
#ifndef APP_DRIVER_TEST
#define APP_DRIVER_TEST             0   /* 1 = print every input event + sound/LED demo at boot */
#endif
#ifndef APP_DEBUG_STATS
#define APP_DEBUG_STATS             1   /* 0 = no periodic stats print in normal mode */
#endif
#ifndef APP_USE_SHOCK_FALLBACK
#define APP_USE_SHOCK_FALLBACK      0   /* 1 = shock switch on PTD6 replaces the ESP32 gyro */
#endif

/* ---- Common register values ---- */
#define APP_GPIO_MUX                1u      /* PORT ALTn = GPIO */
#define APP_ANALOG_MUX              0u      /* PORT ALT0 = analog (ADC) */
#define APP_TPM_ALT3_MUX            3u      /* TPM0_CH2/CH4, TPM1_CH1, TPM2_CH0 on our pins */
#define APP_TPM_ALT4_MUX            4u      /* TPM0_CH5 on PTD5 */
#define APP_IRQC_RISING             0x9u    /* PORT PCR IRQC: interrupt on rising edge */
#define APP_IRQC_FALLING            0xAu    /* PORT PCR IRQC: interrupt on falling edge */
#define APP_IRQC_EITHER             0xBu    /* PORT PCR IRQC: interrupt on either edge */

/* ---- Inter-board link: UART2 on PTE22/PTE23 (proven pins, PORT mux ALT4) ---- */
#define APP_UART_BAUD               115200u
#define APP_UART_TX_PIN             22u     /* PTE22 UART2_TX -> ESP32-S2 GPIO2 (RX) */
#define APP_UART_RX_PIN             23u     /* PTE23 UART2_RX <- ESP32-S2 GPIO1 (TX) */
#define APP_UART_PORT_MUX           4u      /* ALT4 = UART2 on PTE22/23 (RM 10.3.1) */
#define APP_UART_OVERSAMPLE         16u     /* UART2 samples each bit 16 times */
#define APP_UART_SBR_HIGH_SHIFT     8u      /* SBR[12:8] go in BDH, SBR[7:0] in BDL */
#define APP_UART_SBR_LOW_MASK       0xFFu

/* ---- Pins (docs/pin_table.md) ---- */
#define APP_LED_RED_PIN             31u     /* PTE31 TPM0_CH4 (ALT3) */
#define APP_LED_GREEN_PIN           5u      /* PTD5  TPM0_CH5 (ALT4) */
#define APP_LED_BLUE_PIN            29u     /* PTE29 TPM0_CH2 (ALT3) */
#define APP_LED_RED_CH              4u
#define APP_LED_GREEN_CH            5u
#define APP_LED_BLUE_CH             2u
#define APP_JOY_X_PIN               20u     /* PTE20 ADC0_SE0 */
#define APP_JOY_Y_PIN               21u     /* PTE21 ADC0_SE4a */
#define APP_JOY_X_ADC_CH            0u      /* ADC0_SE0 */
#define APP_JOY_Y_ADC_CH            4u      /* ADC0_SE4a (CFG2 MUXSEL = 0 selects 'a') */
#define APP_SW3_PIN                 4u      /* PTA4, on-board SW3 (reset mux is NMI) */
#define APP_ENC_A_PIN               12u     /* PTA12 */
#define APP_ENC_B_PIN               5u      /* PTA5  */
#define APP_ENC_SW_PIN              1u      /* PTC1  */
#define APP_JOY_SW_PIN              2u      /* PTC2  */
#define APP_SHOCK_PIN               6u      /* PTD6 (gyro fallback) */
#define APP_US_TRIG_PIN             3u      /* PTB3 GPIO out */
#define APP_US_ECHO_PIN             13u     /* PTA13 TPM1_CH1 input capture */
#define APP_US_ECHO_CH              1u
#define APP_BUZZER_PIN              2u      /* PTB2 TPM2_CH0 PWM */
#define APP_BUZZER_CH               0u

/* ---- NVIC priorities (Cortex-M0+: 2 bits -> 0/64/128/192, lower = more urgent) ---- */
#define APP_NVIC_PRIO_UART2         64u     /* link bytes beat GPIO so RX never overruns */
#define APP_NVIC_PRIO_TPM1          64u     /* echo edge timestamps are timing-critical */
#define APP_NVIC_PRIO_ADC0          128u    /* conversion done: a 50 Hz sample can wait */
#define APP_NVIC_PRIO_GPIO          128u    /* buttons/encoder: human-speed events */

/* ---- Task priorities (configMAX_PRIORITIES = 5, idle = 0; CLAUDE.md) ---- */
#define APP_PRIO_GAME               4u      /* future game/alarm FSM task (not created yet) */
#define APP_PRIO_JOYSTICK           3u      /* fixed-rate sampling must not jitter */
#define APP_PRIO_COMM               2u      /* link protocol */
#define APP_PRIO_ULTRASONIC         2u      /* equal to comm on purpose: time slicing */
#define APP_PRIO_FEEDBACK           1u      /* buzzer + LED, 10 ms granularity is fine */
#define APP_PRIO_DEBUG              1u      /* stats/prints, lowest app priority */
#define APP_PRIO_LOOPBACK           1u      /* self-test traffic generator */

/* ---- Task stacks (words, 4 bytes each) ---- */
#define APP_STACK_COMM              256u    /* proto_link frame buffers + callbacks */
#define APP_STACK_JOYSTICK          128u
#define APP_STACK_ULTRASONIC        128u
#define APP_STACK_FEEDBACK          160u
#define APP_STACK_DEBUG             288u    /* PRINTF formatting is stack-hungry */
#define APP_STACK_LOOPBACK          256u

/* ---- Queues and buffers ---- */
#define APP_RX_QUEUE_LEN            128u    /* bytes from the UART2 RX ISR (~11 ms at 115200) */
#define APP_TX_RING_SIZE            256u    /* ISR-drained TX ring, holds ~6 max frames */
#define APP_TX_REQ_QUEUE_LEN        4u      /* frames other tasks ask the comm task to send */
#define APP_INPUT_QUEUE_LEN         16u     /* game_event_t from all input drivers */
#define APP_FEEDBACK_QUEUE_LEN      8u      /* sound / tick / LED requests */

/* ---- Comm task timing ---- */
#define APP_COMM_RX_WAIT_MS         10u     /* max wait for a byte before polling timers */

/* ---- Encoder + SW3 ---- */
#define APP_ENC_TRANSITIONS_PER_STEP 4      /* valid quadrature transitions per detent */
#define APP_ENC_INVERT              0       /* 1 = swap the CW/CCW sign */
#define APP_SW3_RING_DELAY_S        3       /* SW3 bench trigger: CMD_RING_NOW value */

/* ---- Buttons ---- */
#define APP_BTN_DEBOUNCE_MS         30u     /* ignore edges this soon after an accepted one */
#define APP_LONG_PRESS_MS           800u    /* encoder press >= this = ENC_LONG_PRESS */
#define APP_SHOCK_TAMPER_MG         2000    /* TAMPER value posted by the shock switch */

/* ---- Joystick (ADC0, 12-bit) ---- */
#define APP_JOY_PERIOD_MS           20u     /* 50 Hz sampling */
#define APP_JOY_ADC_TIMEOUT_MS      5u      /* conversion-done notification timeout */
#define APP_JOY_LOW                 1000u   /* below = pushed to the low side */
#define APP_JOY_HIGH                3000u   /* above = pushed to the high side */
#define APP_JOY_CENTER_LO           1500u   /* re-arm only after returning inside */
#define APP_JOY_CENTER_HI           2600u
#define APP_JOY_X_INVERT            0       /* 0: low raw X = LEFT; 1: low raw X = RIGHT */
#define APP_ADC_CAL_MAX_POLLS       200000u /* bound for the one-time calibration wait */
#define APP_ADC_GAIN_MSB            0x8000u /* RM ADC "Calibration function" (p.392): set the MSB */

/* ---- Ultrasonic (HC-SR04 on TPM1) ---- */
#define APP_US_PERIOD_MS            60u     /* HC-SR04 needs >= 60 ms between pings */
#define APP_US_ECHO_TIMEOUT_MS      35u     /* > 25 ms echo for 400 cm, plus margin */
#define APP_US_TRIG_US              10u     /* trigger pulse width */
#define APP_US_US_PER_CM            58u     /* round-trip microseconds per centimetre */
#define APP_US_MAX_CM               400u    /* HC-SR04 range */
#define APP_US_INVALID_CM           999     /* DISTANCE value for timeout / out of range */

/* ---- TPM clocks: MCGIRCLK = 8 MHz LIRC (BOARD_BootClockRUN), SOPT2 TPMSRC = 3 ---- */
#define APP_TPM_CLK_HZ              8000000u
#define APP_TPMSRC_MCGIRCLK         3u
#define APP_TPM_PS_DIV8             3u      /* PS = 3 -> /8 -> 1 MHz = 1 us per count */
#define APP_TPM_TICK_HZ             1000000u
#define APP_TPM1_MOD                0xFFFFu /* free-running for input capture */
#define APP_TPM_ACK_POLLS           1000u   /* bound for CnSC write-acknowledge waits */

/* ---- Feedback (TPM0 LED PWM, TPM2 buzzer) ---- */
#define APP_FEEDBACK_PERIOD_MS      10u
#define APP_LED_PWM_MOD             999u    /* 1 MHz / 1000 = 1 kHz LED PWM */
#define APP_LED_DIM_PCT             10u     /* ARMED dim blue */
#define APP_LED_FULL_PCT            100u
#define APP_LED_ACTIVE_BLINK_MS     250u    /* red on/off half-period while BOMB_ACTIVE */
#define APP_LED_EXPLODED_SWAP_MS    150u    /* red <-> white alternation */
#define APP_LED_LINKDOWN_PERIOD_MS  2000u   /* LED OFF mode: blue blink while link down */
#define APP_LED_LINKDOWN_ON_MS      100u
#define APP_TICK_CLICK_HZ           2000u   /* bomb timer tick: 30 ms 2 kHz click */
#define APP_TICK_CLICK_MS           30u
#define APP_PERCENT                 100u

/* ---- Debug task ---- */
#define APP_DEBUG_STATS_MS          10000u  /* normal mode stats period */
#define APP_DEBUG_TEST_STATS_MS     5000u   /* driver-test mode stats period */
#define APP_DEBUG_DEMO_STEP_MS      2000u   /* boot demo: one sound + one LED mode per step */

/* ---- Loopback self-test ---- */
#define APP_LOOPBACK_SEND_MS        2000u   /* TIME_SYNC every 2 s */
#define APP_LOOPBACK_PRINT_MS       10000u  /* stats on the debug console every 10 s */

#endif /* APP_CONFIG_H */
