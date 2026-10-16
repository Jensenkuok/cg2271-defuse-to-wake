/*
 * Defuse to Wake - MCXC444 application configuration.
 * Every pin, priority, stack size and queue length lives here (no magic numbers
 * in the .c files). Pin choices and their RM/UM sources: docs/pin_table.md.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "FreeRTOS.h"

/* ---- Build options ---- */
#ifndef APP_LOOPBACK_TEST
#define APP_LOOPBACK_TEST           0   /* 1 = UART2 self-test, jumper PTE22<->PTE23 */
#endif

/* ---- Inter-board link: UART2 on PTE22/PTE23 (proven pins, PORT mux ALT4) ---- */
#define APP_UART_BAUD               115200u
#define APP_UART_TX_PIN             22u     /* PTE22 UART2_TX -> ESP32-S2 GPIO2 (RX) */
#define APP_UART_RX_PIN             23u     /* PTE23 UART2_RX <- ESP32-S2 GPIO1 (TX) */
#define APP_UART_PORT_MUX           4u      /* ALT4 = UART2 on PTE22/23 (RM 10.3.1) */
#define APP_UART_OVERSAMPLE         16u     /* UART2 samples each bit 16 times */
#define APP_UART_SBR_HIGH_SHIFT     8u      /* SBR[12:8] go in BDH, SBR[7:0] in BDL */
#define APP_UART_SBR_LOW_MASK       0xFFu

/* ---- On-board RGB LED (active-low). Plain GPIO for now; TPM0 PWM comes later ---- */
#define APP_LED_RED_PIN             31u     /* PTE31 */
#define APP_LED_GREEN_PIN           5u      /* PTD5  */
#define APP_LED_BLUE_PIN            29u     /* PTE29 */
#define APP_GPIO_MUX                1u      /* ALT1 = GPIO */

/* ---- Pins reserved for later steps (see docs/pin_table.md) ---- */
#define APP_JOY_X_PIN               20u     /* PTE20 ADC0_SE0 */
#define APP_JOY_Y_PIN               21u     /* PTE21 ADC0_SE4a */
#define APP_SW3_PIN                 4u      /* PTA4, on-board SW3 (reset mux is NMI) */
#define APP_ENC_A_PIN               12u     /* PTA12 GPIO IRQ */
#define APP_ENC_B_PIN               5u      /* PTA5  GPIO IRQ */
#define APP_ENC_SW_PIN              1u      /* PTC1  GPIO IRQ */
#define APP_JOY_SW_PIN              2u      /* PTC2  GPIO IRQ */
#define APP_SHOCK_PIN               6u      /* PTD6  GPIO IRQ (gyro fallback) */
#define APP_US_TRIG_PIN             3u      /* PTB3  GPIO out */
#define APP_US_ECHO_PIN             13u     /* PTA13 TPM1_CH1 input capture, ALT3 */
#define APP_BUZZER_PIN              2u      /* PTB2  TPM2_CH0 PWM, ALT3 */

/* ---- NVIC priorities (Cortex-M0+: 2 bits -> 0/64/128/192, lower = more urgent) ---- */
#define APP_NVIC_PRIO_UART2         64u     /* link bytes beat GPIO so RX never overruns */
#define APP_NVIC_PRIO_GPIO          128u    /* buttons/encoder: human-speed events */

/* ---- Task priorities (configMAX_PRIORITIES = 5, idle = 0) ---- */
#define APP_PRIO_GAME               (configMAX_PRIORITIES - 1)  /* future game/alarm FSM task */
#define APP_PRIO_COMM               (APP_PRIO_GAME - 1)         /* just below the game task */
#define APP_PRIO_LOOPBACK           (tskIDLE_PRIORITY + 1)      /* self-test, lowest app task */

/* ---- Task stacks (words, 4 bytes each) ---- */
#define APP_STACK_COMM              256u    /* proto_link frame buffers + callbacks */
#define APP_STACK_LOOPBACK          256u    /* PRINTF formatting is stack-hungry */

/* ---- Queues and buffers ---- */
#define APP_RX_QUEUE_LEN            128u    /* bytes from the UART2 RX ISR (~11 ms at 115200) */
#define APP_TX_RING_SIZE            256u    /* ISR-drained TX ring, holds ~6 max frames */
#define APP_TX_REQ_QUEUE_LEN        4u      /* frames other tasks ask the comm task to send */

/* ---- Comm task timing ---- */
#define APP_COMM_RX_WAIT_MS         10u     /* max wait for a byte before polling timers */

/* ---- Loopback self-test ---- */
#define APP_LOOPBACK_SEND_MS        2000u   /* TIME_SYNC every 2 s */
#define APP_LOOPBACK_PRINT_MS       10000u  /* stats on the debug console every 10 s */

#endif /* APP_CONFIG_H */
