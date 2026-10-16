/*
 * Copyright 2016-2025 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause=====
 */

/**
 * @file    CG2271RTOS_UART.c
 * @brief   Application entry point.
 */
#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
/* TODO: insert other include files here. */
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include <stdlib.h>

/* TODO: insert other definitions and declarations here. */

#define BAUD_RATE 9600
#define UART_TX_PTE22 	22
#define UART_RX_PTE23 	23
#define UART2_INT_PRIO	128

#define MAX_MSG_LEN		256
char send_buffer[MAX_MSG_LEN];

#define QLEN	5
QueueHandle_t queue;
typedef struct tm {
	char message[MAX_MSG_LEN];
} TMessage;

// ---- Demo: LED actuator and shared state ----
#define RED_PIN         31   // PTE31
#define GREEN_PIN       5    // PTD5
#define HIGH_THRESHOLD 3000   // pushed one way
#define LOW_THRESHOLD  1000   // pushed the other way

typedef enum { RED, GREEN } TLED;
SemaphoreHandle_t newData;
volatile int lightLevel = 0;

void initLEDs(void) {
	SIM->SCGC5 |= (SIM_SCGC5_PORTD_MASK | SIM_SCGC5_PORTE_MASK);
	PORTE->PCR[RED_PIN] &= ~PORT_PCR_MUX_MASK;
	PORTE->PCR[RED_PIN] |= PORT_PCR_MUX(1);
	PORTD->PCR[GREEN_PIN] &= ~PORT_PCR_MUX_MASK;
	PORTD->PCR[GREEN_PIN] |= PORT_PCR_MUX(1);
	GPIOE->PDDR |= (1 << RED_PIN);
	GPIOD->PDDR |= (1 << GREEN_PIN);
}

// LEDs are active-low: clearing the pin turns the LED on
void setLED(TLED led, int on) {
	if(led == RED) {
		if(on) GPIOE->PCOR = (1 << RED_PIN); else GPIOE->PSOR = (1 << RED_PIN);
	} else {
		if(on) GPIOD->PCOR = (1 << GREEN_PIN); else GPIOD->PSOR = (1 << GREEN_PIN);
	}
}

void initUART2(uint32_t baud_rate)
{
	NVIC_DisableIRQ(UART2_FLEXIO_IRQn);

	//enable clock to UART2 and PORTE
	SIM->SCGC4 |= SIM_SCGC4_UART2_MASK;
	SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;

	//Ensure Tx and Rx are disabled before configuration
	UART2->C2 &= ~((UART_C2_TE_MASK) | (UART_C2_RE_MASK));

	//connect UART pins for PTE22, PTE23
	PORTE->PCR[UART_TX_PTE22] &= ~PORT_PCR_MUX_MASK;
	PORTE->PCR[UART_TX_PTE22] |= PORT_PCR_MUX(4);

	PORTE->PCR[UART_RX_PTE23] &= ~PORT_PCR_MUX_MASK;
	PORTE->PCR[UART_RX_PTE23] |= PORT_PCR_MUX(4);

	// Set the baud rate
	uint32_t bus_clk = CLOCK_GetBusClkFreq();

	// This version of sbr does integer rounding.
	uint32_t sbr = (bus_clk + (baud_rate * 8)) / (baud_rate * 16);

	// Set SBR. Bits 8 to 12 in BDH, 0-7 in BDL.
	// MUST SET BDH FIRST!
	UART2->BDH &= ~UART_BDH_SBR_MASK;
	UART2->BDH |= ((sbr >> 8) & UART_BDH_SBR_MASK);
	UART2->BDL = (uint8_t) (sbr &0xFF);

	// Disable loop mode
	UART2->C1 &= ~UART_C1_LOOPS_MASK;
	UART2->C1 &= ~UART_C1_RSRC_MASK;

	// Disable parity
	UART2->C1 &= ~UART_C1_PE_MASK;

	// 8-bit mode
	UART2->C1 &= ~UART_C1_M_MASK;

	//Enable RX interrupt
	UART2->C2 |= UART_C2_RIE_MASK;

	// Enable the receiver
	UART2->C2 |= UART_C2_RE_MASK;

	NVIC_SetPriority(UART2_FLEXIO_IRQn, UART2_INT_PRIO);
	NVIC_ClearPendingIRQ(UART2_FLEXIO_IRQn);
	NVIC_EnableIRQ(UART2_FLEXIO_IRQn);

}


void UART2_FLEXIO_IRQHandler(void)
{
	// Send and receive pointers
	static int recv_ptr=0, send_ptr=0;
	char rx_data;
	static char recv_buffer[MAX_MSG_LEN];

//NVIC_ClearPendingIRQ(UART2_FLEXIO_IRQn);
	if(UART2->S1 & UART_S1_TDRE_MASK) // Send data
	{
		if(send_buffer[send_ptr] == '\0') {
			send_ptr = 0;

			// Disable the transmit interrupt
			UART2->C2 &= ~UART_C2_TIE_MASK;

			// Disable the transmitter
			UART2->C2 &= ~UART_C2_TE_MASK;
		}
		else {
			UART2->D = send_buffer[send_ptr++];
		}
	}

	if(UART2->S1 & UART_S1_RDRF_MASK)
	{
		TMessage msg;
		rx_data = UART2->D;
		recv_buffer[recv_ptr++] = rx_data;
		if(rx_data == '\n') {
			// Copy over the string
			BaseType_t hpw = pdFALSE;
			recv_buffer[recv_ptr]='\0';
			strncpy(msg.message, recv_buffer, MAX_MSG_LEN);
			xQueueSendFromISR(queue, (void *)&msg, &hpw);
			portYIELD_FROM_ISR(hpw);
			recv_ptr = 0;
		}
	}

}

void sendMessage(char *message) {
	strncpy(send_buffer, message, MAX_MSG_LEN);

	// Enable the TIE interrupt
	UART2->C2 |= UART_C2_TIE_MASK;

	// Enable the transmitter
	UART2->C2 |= UART_C2_TE_MASK;
}

// Priority 3: communications (handles each incoming message)
static void recvTask(void *p) {
	while(1) {
		TMessage msg;
		if(xQueueReceive(queue, &msg, portMAX_DELAY) == pdTRUE) {
			if(msg.message[0] == 'L' && msg.message[1] == ':') {
				lightLevel = atoi(&msg.message[2]);
				PRINTF("Joystick level: %d\r\n", lightLevel);
				xSemaphoreGive(newData);
			}
		}
	}
}

// Priority 2: actuator (waits for new data, then updates the LED)
static void ledTask(void *p) {
	while(1) {
		if(xSemaphoreTake(newData, portMAX_DELAY) == pdTRUE) {
			int bright = (lightLevel >= HIGH_THRESHOLD) || (lightLevel <= LOW_THRESHOLD);
			setLED(GREEN, bright);
			setLED(RED, !bright);
		}
	}
}
/*
 * @brief   Application entry point.
 */
int main(void) {

    /* Init board hardware. */
	BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
    /* Init FSL debug console. */
    BOARD_InitDebugConsole();
#endif

    initUART2(9600);
    PRINTF("UART2 DEMO\r\n");

    queue = xQueueCreate(QLEN, sizeof(TMessage));

    newData = xSemaphoreCreateBinary();
    initLEDs();
    setLED(RED, 0);
    setLED(GREEN, 0);

    xTaskCreate(recvTask, "recvTask", configMINIMAL_STACK_SIZE+100, NULL, 3, NULL);
    xTaskCreate(ledTask,  "ledTask",  configMINIMAL_STACK_SIZE+100, NULL, 2, NULL);

    vTaskStartScheduler();

    /* Force the counter to be placed into memory. */
    volatile static int i = 0 ;
    /* Enter an infinite loop, just incrementing a counter. */
    while(1) {
        i++ ;
        /* 'Dummy' NOP to allow source level single stepping of
            tight while() loop */
        __asm volatile ("nop");
    }
    return 0 ;
}

