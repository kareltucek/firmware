#ifndef __UART_H__
#define __UART_H__

#include <stdint.h>
#include "fsl_port.h"
#include "module/i2c.h"

// LPUART0 mapped onto the former I2C bus pins (PTB3/PTB4) on KL03.
#define UART_BUS_TX_PORT  PORTB
#define UART_BUS_TX_PIN   3
#define UART_BUS_RX_PORT  PORTB
#define UART_BUS_RX_PIN   4
#define UART_BUS_PIN_MUX  kPORT_MuxAlt3
#define UART_BUS_BAUD     9600U

void ModuleUart_RequestKeyStatesUpdate(void);
void ModuleUart_Loop(void);
void InitModuleUart(void);

#endif

