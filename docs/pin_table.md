# MCXC444 pin table (FRDM-MCXC444, MCXC444VLH 64 LQFP)

Sources:
- **RM**: MCX C44X Sub-Family Reference Manual, Rev. 1, 07/2024 (`MCXC44XP64M48RM.pdf`).
  - §10.3.1 *MCX C44x Pin Assignments* table (pp. 119–121) gives the ALTn mux values used below.
  - **Table 11-1 *Ports summary*** (pp. 140–141), row *Interrupt and DMA request*: Port A = Yes,
    Port B = No, Port C = Yes, Port D = Yes, Port E = No. So **only Ports A, C and D can raise pin
    interrupts**. NVIC vectors: `PORTA_IRQn` (30) and the shared `PORTC_PORTD_IRQn` (31).
  - §4.3.1 table (p. 61): MCXC444VLH has **32 KB SRAM**.
- **UM**: UM12120 FRDM-MCXC444 Board User Manual, Rev. 1, 15 July 2024 (`UM12120.pdf`).
  - Table 16 (J1), Table 17 (J2), Table 18 (J3, power), Table 19 (J4) on pp. 19–21: header pins
    and the "Potential conflict" column.
  - Table 13 (on-board segment LCD DS1, pp. 16–17), §2.6/Table 11 (I2C, pp. 13–15),
    §2.8 accelerometer on PTE24/PTE25 (p. 17), §2.9 light sensor on PTE22 (p. 18),
    §2.4/Table 9 debug console LPUART0 on PTA1/PTA2 (pp. 10–11), Table 5 buttons (p. 8).

Rows marked **proven** come from the team's Lab 2/3 code (CLAUDE.md) and are not changed.

| Signal | MCU pin | PORT mux / function | FRDM header + pin | Direction / voltage | Why this pin |
|---|---|---|---|---|---|
| UART2 TX → ESP32 RX (GPIO2) | PTE22 | ALT4 UART2_TX (RM p.119) | J4[5] (UM Table 19) | Out, 3.3 V | **Proven.** UM Table 19 lists the on-board ambient light sensor (Light_sns, isolated by 0 Ω R82, UM §2.9) on this pin; it worked in Lab 3, leave R82 alone. |
| UART2 RX ← ESP32 TX (GPIO1) | PTE23 | ALT4 UART2_RX (RM p.119) | J4[7] (UM Table 19) | In, 3.3 V | **Proven.** No on-board conflict listed. |
| RGB LED red | PTE31 | ALT3 TPM0_CH4 (RM p.119) | not on a header (on-board LED D3, UM Table 6) | Out, active-low | **Proven.** TPM0 is reserved for the LED. |
| RGB LED green | PTD5 | ALT4 TPM0_CH5 (RM p.121) | J2[12] (UM Table 17) | Out, active-low | **Proven.** Also J2 "D13/SCK/LED". |
| RGB LED blue | PTE29 | ALT3 TPM0_CH2 (RM p.119) | J1[14] (UM Table 16) | Out, active-low | **Proven.** |
| Joystick X | PTE20 | ALT0 ADC0_SE0 (default, RM p.119) | J4[1] (UM Table 19) | Analog in, 0–3.3 V | **Proven.** UM Table 19/Table 13: also SLCD COM0 (LCD-01_P59); the SLCD stays disabled. |
| Joystick Y | PTE21 | ALT0 ADC0_SE4a (default, RM p.119) | J4[3] (UM Table 19) | Analog in, 0–3.3 V | **Proven.** Also SLCD COM1 (LCD-02_P60); SLCD stays disabled. |
| On-board SW3 | PTA4 | ALT1 GPIO (reset default is ALT7 NMI_b, RM p.119) | J1[10] (UM Table 16) | In | **Proven.** Must be re-muxed to ALT1 before use or it fires NMI. |
| Encoder A | PTA12 | ALT1 GPIO, IRQC either edge (RM p.119) | J1[8] (UM Table 16) | In, 3.3 V, pull-up | Port A has pin interrupts (RM Table 11-1). Only conflict listed is the empty mikroBUS socket J5 pin 1. A and B share the `PORTA_IRQn` vector, so one ISR reads both levels. |
| Encoder B | PTA5 | ALT1 GPIO, IRQC either edge (RM p.119) | J1[12] (UM Table 16) | In, 3.3 V, pull-up | Port A, interrupt-capable. Only conflict listed is Pmod J7, which is not populated (DNP). |
| Encoder SW (push) | PTC1 | ALT1 GPIO, IRQC falling edge (RM p.120) | J4[12] (UM Table 19) | In, 3.3 V, pull-up | Port C has pin interrupts. Conflicts listed: Pmod J7 (DNP) and mikroBUS J5 pin 5 (empty socket). I2C1 SCL alt function is not used. The accelerometer is on I2C0 PTE24/25 (UM §2.8), not here. |
| Joystick SW | PTC2 | ALT1 GPIO, IRQC falling edge (RM p.120) | J4[10] (UM Table 19) | In, 3.3 V, pull-up | Port C, interrupt-capable. Same conflicts as PTC1 (DNP Pmod, empty mikroBUS). |
| Shock switch (gyro fallback) | PTD6 | ALT1 GPIO, IRQC falling edge (RM p.121) | J2[8] (UM Table 17) | In, 3.3 V, pull-up | Port D, interrupt-capable (shares `PORTC_PORTD_IRQn` with the buttons). Conflicts listed: Pmod J7 (DNP), mikroBUS J6 pin 6 (empty). Not used by the SLCD (UM Table 13 lists PTD0/2/3/4 only). |
| HC-SR04 TRIG | PTB3 | ALT1 GPIO out (RM p.120/121) | J4[8] (UM Table 19) | Out, 3.3 V logic into a 5 V module | Plain output; no interrupt needed, so Port B is fine. UM Table 19 lists no conflict. |
| HC-SR04 ECHO | PTA13 | ALT3 TPM1_CH1 input capture (RM p.119) | J2[2] (UM Table 17, "D8 / InputCapture") | In, **5 V echo through a divider to ≤ 3.3 V** | Needs a TPM capture channel not on TPM0 (LED). TPM1 is free and runs as a free-running timer for capture. UM Table 17 lists no conflict. |
| Passive buzzer | PTB2 | ALT3 TPM2_CH0 PWM (RM p.120/121) | J4[6] (UM Table 19) | Out, 3.3 V | Needs PWM not on TPM0 (LED) and not on TPM1. TPM1 must free-run for ECHO capture, while the buzzer changes MOD to set the tone. UM Table 19 lists no conflict. |
| Debug console (not ours) | PTA1 / PTA2 | ALT2 LPUART0 RX/TX | MCU-Link VCOM (UM Table 9) | – | Reserved for PRINTF; nothing above uses PTA1/PTA2. |

## Timer allocation
- **TPM0**: RGB LED PWM (CH2 blue, CH4 red, CH5 green). Proven.
- **TPM1**: HC-SR04 echo input capture on CH1 (PTA13).
- **TPM2**: buzzer PWM on CH0 (PTB2).
- TPM clock source: MCGIRCLK (SIM->SOPT2 TPMSRC = 3), shared by all TPMs. Proven.

## Wiring and voltage notes
- **HC-SR04 ECHO is a 5 V signal.** Feed it through a resistor divider (for example 1 kΩ in series,
  2 kΩ to GND, giving ≈ 3.3 V) before PTA13. Power the HC-SR04 from P5V0 on J3[10] (UM Table 18).
- **TRIG at 3.3 V:** whether the HC-SR04 accepts 3.3 V logic on TRIG is not covered by either PDF.
  UNVERIFIED; test on the bench.
- **Joystick and encoder** run from **3.3 V** (P3V3 on J3[8], UM Table 18), so their outputs never
  exceed the MCU's 3.3 V I/O. GND is on J3[4]/J3[12]/J3[14] or J2[14].
- Both boards are on their own USB; tie the grounds together (CLAUDE.md wiring rules).
- Button and encoder inputs use the internal pull-up (PCR PE = 1, PS = 1), so switches connect to GND.

## UNVERIFIED
- 3.3 V TRIG drive into the HC-SR04 (not in the RM or UM).
- No 5 V tolerance claim is made for any pin; the ECHO divider is required.
