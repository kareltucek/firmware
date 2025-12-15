#include <stdbool.h>
#include <stdint.h>
#include "fsl_port.h"
#include "fsl_clock.h"
#include "fsl_lpuart.h"
#include "module/i2c.h"
#include "module/uart.h"
#include "uart.h"
#include "module.h"
#include "slave_protocol_handler.h"
#include "uart_parser.h"
#include "slave_protocol.h"
#include "module/slave_protocol_handler.h"

#define CRC_LEN 2

lpuart_config_t uartConfig;
lpuart_handle_t uartHandle;
lpuart_transfer_t uartTransfer;

#define UART_BUFF_SIZE UART_MAX_SERIALIZED_MESSAGE_LENGTH
uint8_t uartRxBuffer[UART_MAX_SERIALIZED_MESSAGE_LENGTH];
uint16_t uartRxReadCount = 0;

bool hasRawIncomingMessage = false;
bool hasValidIncomingMessage = false;

static uart_parser_t uartParser;


uint32_t get_stack_size(void)
{
    extern char __StackTop;
    extern char __StackLimit;
    return (uint32_t)&__StackTop - (uint32_t)&__StackLimit;
}

uint32_t get_stack_used(void)
{
    extern char __StackTop;
    uint32_t sp;
    __asm volatile ("MOV %0, SP" : "=r" (sp));
    return (uint32_t)&__StackTop - sp;
}

static void outputStacks() {
    uint8_t s = get_stack_size() / 4;
    uint8_t u = get_stack_used() / 4;

    debugOut[NEXTPOS] = 1;
    debugOut[NEXTPOS] = s;
    debugOut[NEXTPOS] = u;
}

static inline void handleSlaveProtocolMessage(uint16_t dataOffset) {
    TxMessage.length = 0;

    // they read the rx buffer and write the tx buffer
    if (IsI2cRxTransaction(RxMessage.data[dataOffset])) {
        SlaveRxHandler(dataOffset);
    } else {
        SlaveTxHandler(dataOffset);
    }

    UartParser_SetTxBuffer(&uartParser, uartRxBuffer);
    UartParser_StartMessage(&uartParser);
    UartParser_AppendEscapedTxBytes(&uartParser, TxMessage.data, TxMessage.length);
    UartParser_FinalizeMessage(&uartParser);

    LPUART_WriteBlocking(LPUART0, uartParser.txBuffer, uartParser.txPosition);
}

static void processRxBuffer(void) {
    if (uartRxReadCount > 0) {
        UartParser_ProcessIncomingBytes(&uartParser, uartRxBuffer, uartRxReadCount);
        memset(uartRxBuffer, 0, uartRxReadCount);
        uartRxReadCount = 0;
    }
}

static void startListening(void) {
    uartTransfer.data = uartRxBuffer;
    uartTransfer.dataSize = UART_BUFF_SIZE;
    LPUART_TransferReceiveNonBlocking(LPUART0, &uartHandle, &uartTransfer, NULL);
}

void ModuleUart_Loop(void)
{
    if (hasRawIncomingMessage) {
        outputStacks();
        hasRawIncomingMessage = false;
        processRxBuffer();

        if (hasValidIncomingMessage) {
            hasValidIncomingMessage = false;
            handleSlaveProtocolMessage(CRC_LEN);
        }
        startListening();
    }
}


static void processDeserializedRxData(void *state, uart_control_t messageKind, const uint8_t* data, uint16_t len) {
    switch (messageKind) {
        case UartControl_Ack:
        case UartControl_Nack:
        case UartControl_Ping:
        case UartControl_InvalidMessage:
        case UartControl_Unexpected:
            break;
        case UartControl_ValidMessage:
            // continues in handleSlaveProtocolMessage, but pops a few calls from the stack first
            hasValidIncomingMessage = true;
            RxMessage.length = len;
            break;
    }
}

static void lpuartCallback(LPUART_Type *base, lpuart_handle_t *handle, status_t status, void *arg1)
{
    if (status == kStatus_LPUART_RxIdle || status == kStatus_LPUART_IdleLineDetected) {
        uartRxReadCount = uartHandle.rxDataSizeAll - uartHandle.rxDataSize;
    }

    // The lpuart functions need to be called from the main thread.
    hasRawIncomingMessage = true;
}

static void initLpUart(void) {
    CLOCK_EnableClock(I2C_BUS_SDA_CLOCK);
    CLOCK_EnableClock(I2C_BUS_SCL_CLOCK);

    CLOCK_SetLpuart0Clock(1U);
    uint32_t lpuartClock = 48*1000*1000;


    PORT_SetPinMux(UART_BUS_TX_PORT, UART_BUS_TX_PIN, UART_BUS_PIN_MUX);
    PORT_SetPinMux(UART_BUS_RX_PORT, UART_BUS_RX_PIN, UART_BUS_PIN_MUX);

    LPUART_GetDefaultConfig(&uartConfig);
    uartConfig.enableRx = true;
    uartConfig.enableTx = true;
    uartConfig.baudRate_Bps = 9600U;

    LPUART_Init(LPUART0, &uartConfig, lpuartClock);
    LPUART_TransferCreateHandle(LPUART0, &uartHandle, lpuartCallback, NULL);

    startListening();
}

static void initUartParser(void) {
    UartParser_InitParser(&uartParser, &processDeserializedRxData, NULL);
    UartParser_SetRxBuffer(&uartParser, RxMessage.data);
    UartParser_SetTxBuffer(&uartParser, RxMessage.data);
}

void InitModuleUart(void)
{
    initLpUart();
    initUartParser();
}

