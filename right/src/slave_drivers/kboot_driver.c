#include "slave_drivers/kboot_driver.h"
#include "slave_drivers/uhk_module_driver.h"
#include "slave_scheduler.h"
#include "i2c.h"
#include "i2c_addresses.h"
#include "logger.h"
#include "timer.h"
#include "crc16.h"
#include "config_parser/config_globals.h"
#include "module_flash.h"
#include <string.h>

kboot_driver_state_t KbootDriverState;

static uint8_t rxBuffer[KBOOT_PACKAGE_MAX_LENGTH];
static uint8_t txBuffer[6 + KBOOT_PACKAGE_MAX_LENGTH];
static uint8_t pingCommand[] = {0x5a, 0xa6};
static uint8_t resetCommand[] = {0x5a, 0xa4, 0x04, 0x00, 0x6f, 0x46, 0x0b, 0x00, 0x00, 0x00};
static uint8_t ackMessage[] = {0x5a, 0xa1};
static uint8_t erasePayload[] = {0x0d, 0x00, 0x00, 0x00};

#define KBOOT_WAIT_AFTER_JUMP_MS 10
#define KBOOT_PING_TIMEOUT_MS    20000
#define KBOOT_DEFAULT_I2C_ADDRESS 0x10
#define KBOOT_DATA_CHUNK_SIZE    32
#define KBOOT_ERASE_TIMEOUT_MS   10000
#define KBOOT_PROGRESS_LOG_BYTES 8192

static status_t tx(uint8_t *buffer, uint8_t length)
{
    return I2cAsyncWrite(KbootDriverState.i2cAddress, buffer, length);
}

static status_t rx(uint8_t length)
{
    return I2cAsyncRead(KbootDriverState.i2cAddress, rxBuffer, length);
}

static uint32_t elapsedMs(void)
{
    return Timer_GetCurrentTime() - KbootDriverState.startTime;
}

static uint8_t buildFramingPacket(uint8_t packetType, const uint8_t *payload, uint16_t len)
{
    txBuffer[0] = 0x5a;
    txBuffer[1] = packetType;
    txBuffer[2] = len & 0xff;
    txBuffer[3] = (len >> 8) & 0xff;

    crc16_data_t crc;
    crc16_init(&crc);
    crc16_update(&crc, txBuffer, 4);
    crc16_update(&crc, payload, len);
    uint16_t crcValue;
    crc16_finalize(&crc, &crcValue);

    txBuffer[4] = crcValue & 0xff;
    txBuffer[5] = (crcValue >> 8) & 0xff;
    memcpy(txBuffer + 6, payload, len);

    return 6 + len;
}

static void logResponsePacket(const char *label)
{
    LogU("Kboot: %s response:", label);
    for (uint8_t i = 0; i < KBOOT_PACKAGE_LENGTH_GENERIC_RESPONSE; i++) {
        LogU(" %02x", rxBuffer[i]);
    }
    LogU("\n");
}

static uint32_t getResponseStatus(void)
{
    return rxBuffer[10] | ((uint32_t)rxBuffer[11] << 8) |
           ((uint32_t)rxBuffer[12] << 16) | ((uint32_t)rxBuffer[13] << 24);
}

static void togglePingAddress(void)
{
    KbootDriverState.i2cAddress = KbootDriverState.i2cAddress == I2C_ADDRESS_RIGHT_MODULE_BOOTLOADER
        ? KBOOT_DEFAULT_I2C_ADDRESS
        : I2C_ADDRESS_RIGHT_MODULE_BOOTLOADER;
}

static void abortFlash(const char *reason)
{
    LogU("Kboot: ABORT - %s\n", reason);
    ModuleFlashState = ModuleFlashState_Error;
    ModuleFlashBusy = false;
    KbootDriverState.command = KbootCommand_Idle;
}

void KbootSlaveDriver_Init(uint8_t kbootInstanceId)
{
}

slave_result_t KbootSlaveDriver_Update(uint8_t kbootInstanceId)
{
    slave_result_t res = { .status = kStatus_Uhk_IdleSlave, .hold = false };

    switch (KbootDriverState.command) {
        case KbootCommand_Idle:
            break;
        case KbootCommand_Ping:
            switch (KbootDriverState.phase) {
                case KbootPhase_SendPing:
                    res.status = tx(pingCommand, sizeof(pingCommand));
                    KbootDriverState.phase = KbootPhase_CheckPingStatus;
                    break;
                case KbootPhase_CheckPingStatus:
                    KbootDriverState.status = Slaves[SlaveId_KbootDriver].previousStatus;
                    KbootDriverState.phase = KbootDriverState.status == kStatus_Success
                        ? KbootPhase_ReceivePingResponse
                        : KbootPhase_SendPing;
                    res.status =  kStatus_Uhk_IdleCycle;
                    res.hold = true;
                    break;
                case KbootPhase_ReceivePingResponse:
                    res.status = rx(KBOOT_PACKAGE_LENGTH_PING_RESPONSE);
                    KbootDriverState.phase = KbootPhase_CheckPingResponseStatus;
                    break;
                case KbootPhase_CheckPingResponseStatus:
                    KbootDriverState.status = Slaves[SlaveId_KbootDriver].previousStatus;
                    if (KbootDriverState.status == kStatus_Success) {
                        KbootDriverState.command = KbootCommand_Idle;
                    } else {
                        KbootDriverState.phase = KbootPhase_SendPing;
                        res.status =  kStatus_Uhk_IdleCycle;
                        res.hold = true;
                    }
                    break;
                }
            break;
        case KbootCommand_Reset:
            switch (KbootDriverState.phase) {
                case KbootPhase_SendReset:
                    res.status = tx(resetCommand, sizeof(resetCommand));
                    KbootDriverState.phase = KbootPhase_ReceiveResetAck;
                    break;
                case KbootPhase_ReceiveResetAck:
                    res.status = rx(KBOOT_PACKAGE_LENGTH_ACK);
                    KbootDriverState.phase = KbootPhase_ReceiveResetGenericResponse;
                    break;
                case KbootPhase_ReceiveResetGenericResponse:
                    res.status = rx(KBOOT_PACKAGE_LENGTH_GENERIC_RESPONSE);
                    KbootDriverState.phase = KbootPhase_CheckResetSendAck;
                    break;
                case KbootPhase_CheckResetSendAck:
                    res.status = tx(ackMessage, sizeof(ackMessage));
                    KbootDriverState.command = KbootCommand_Idle;
                    break;
            }
            break;
        case KbootCommand_Flash:
            switch (KbootDriverState.phase) {
                case KbootFlashPhase_JumpToBootloader: {
                    config_buffer_t *buf = ConfigBufferIdToConfigBuffer(ConfigBufferId_ModuleFirmware);
                    KbootDriverState.firmwareData = buf->buffer;
                    KbootDriverState.firmwareSize = ModuleFirmwareValidatedSize;
                    if (KbootDriverState.firmwareSize == 0) {
                        abortFlash("No validated firmware (size=0)");
                        break;
                    }

                    LogU("Kboot: Firmware ready, %u bytes. Jumping to bootloader\n", KbootDriverState.firmwareSize);
                    UhkModuleStates[UhkModuleDriverId_RightModule].phase = UhkModulePhase_JumpToBootloader;
                    KbootDriverState.i2cAddress = I2C_ADDRESS_RIGHT_MODULE_BOOTLOADER;
                    KbootDriverState.startTime = Timer_GetCurrentTime();
                    KbootDriverState.phase = KbootFlashPhase_WaitForBootloader;
                    break;
                }
                case KbootFlashPhase_WaitForBootloader:
                    if (elapsedMs() < KBOOT_WAIT_AFTER_JUMP_MS) {
                        break;
                    }
                    LogU("Kboot: Wait done (%dms), pinging bootloader at 0x%02x/0x%02x\n",
                         KBOOT_WAIT_AFTER_JUMP_MS, I2C_ADDRESS_RIGHT_MODULE_BOOTLOADER,
                         KBOOT_DEFAULT_I2C_ADDRESS);
                    KbootDriverState.phase = KbootFlashPhase_SendPing;
                    break;
                case KbootFlashPhase_SendPing:
                    res.status = tx(pingCommand, sizeof(pingCommand));
                    KbootDriverState.phase = KbootFlashPhase_CheckPingStatus;
                    break;
                case KbootFlashPhase_CheckPingStatus:
                    KbootDriverState.status = Slaves[SlaveId_KbootDriver].previousStatus;
                    if (KbootDriverState.status == kStatus_Success) {
                        KbootDriverState.phase = KbootFlashPhase_ReceivePingResponse;
                    } else if (elapsedMs() > KBOOT_PING_TIMEOUT_MS) {
                        LogU("Kboot: Ping timed out after %dms (last status=%d)\n",
                             KBOOT_PING_TIMEOUT_MS, KbootDriverState.status);
                        KbootDriverState.command = KbootCommand_Idle;
                        break;
                    } else {
                        togglePingAddress();
                        KbootDriverState.phase = KbootFlashPhase_SendPing;
                    }
                    res.status = kStatus_Uhk_IdleCycle;
                    res.hold = true;
                    break;
                case KbootFlashPhase_ReceivePingResponse:
                    res.status = rx(KBOOT_PACKAGE_LENGTH_PING_RESPONSE);
                    KbootDriverState.phase = KbootFlashPhase_CheckPingResponseStatus;
                    break;
                case KbootFlashPhase_CheckPingResponseStatus: {
                    KbootDriverState.status = Slaves[SlaveId_KbootDriver].previousStatus;
                    if (KbootDriverState.status == kStatus_Success) {
                        LogU("Kboot: Bootloader ping OK at 0x%02x (%dms after jump)\n",
                             KbootDriverState.i2cAddress, elapsedMs());
                        ModuleFlashState = ModuleFlashState_Erasing;
                        KbootDriverState.phase = KbootFlashPhase_SendEraseCommand;
                    } else if (elapsedMs() > KBOOT_PING_TIMEOUT_MS) {
                        LogU("Kboot: Ping response timed out after %dms (last status=%d)\n",
                             KBOOT_PING_TIMEOUT_MS, KbootDriverState.status);
                        KbootDriverState.command = KbootCommand_Idle;
                        break;
                    } else {
                        togglePingAddress();
                        KbootDriverState.phase = KbootFlashPhase_SendPing;
                    }
                    res.status = kStatus_Uhk_IdleCycle;
                    res.hold = true;
                    break;
                }

                // Erase phases
                case KbootFlashPhase_SendEraseCommand: {
                    uint8_t len = buildFramingPacket(0xa4, erasePayload, sizeof(erasePayload));
                    res.status = tx(txBuffer, len);
                    KbootDriverState.phase = KbootFlashPhase_ReceiveEraseAck;
                    break;
                }
                case KbootFlashPhase_ReceiveEraseAck:
                    res.status = rx(KBOOT_PACKAGE_LENGTH_ACK);
                    KbootDriverState.phase = KbootFlashPhase_ReceiveEraseResponse;
                    break;
                case KbootFlashPhase_ReceiveEraseResponse:
                    res.status = rx(KBOOT_PACKAGE_LENGTH_GENERIC_RESPONSE);
                    KbootDriverState.phase = KbootFlashPhase_SendEraseResponseAck;
                    break;
                case KbootFlashPhase_SendEraseResponseAck: {
                    if (rxBuffer[0] != 0x5a || rxBuffer[1] != 0xa4) {
                        if (elapsedMs() > KBOOT_ERASE_TIMEOUT_MS) {
                            logResponsePacket("Erase(timeout)");
                            abortFlash("Erase response timeout");
                            break;
                        }
                        KbootDriverState.phase = KbootFlashPhase_ReceiveEraseResponse;
                        res.status = kStatus_Uhk_IdleCycle;
                        res.hold = true;
                        break;
                    }
                    logResponsePacket("Erase");
                    uint32_t status = getResponseStatus();
                    if (status != 0) {
                        LogU("Kboot: Erase failed (status=0x%x)\n", status);
                        abortFlash("Erase failed");
                        break;
                    }
                    res.status = tx(ackMessage, sizeof(ackMessage));
                    LogU("Kboot: Erase complete\n");
                    KbootDriverState.firmwareOffset = 0;
                    KbootDriverState.phase = KbootFlashPhase_SendWriteCommand;
                    break;
                }

                // WriteMemory command phases
                case KbootFlashPhase_SendWriteCommand: {
                    uint8_t writePayload[12];
                    writePayload[0] = 0x04;
                    writePayload[1] = 0x01;
                    writePayload[2] = 0x00;
                    writePayload[3] = 0x02;
                    writePayload[4] = 0x00;
                    writePayload[5] = 0x00;
                    writePayload[6] = 0x00;
                    writePayload[7] = 0x00;
                    uint32_t size = KbootDriverState.firmwareSize;
                    writePayload[8] = size & 0xff;
                    writePayload[9] = (size >> 8) & 0xff;
                    writePayload[10] = (size >> 16) & 0xff;
                    writePayload[11] = (size >> 24) & 0xff;
                    uint8_t len = buildFramingPacket(0xa4, writePayload, sizeof(writePayload));
                    res.status = tx(txBuffer, len);
                    KbootDriverState.phase = KbootFlashPhase_ReceiveWriteAck;
                    break;
                }
                case KbootFlashPhase_ReceiveWriteAck:
                    res.status = rx(KBOOT_PACKAGE_LENGTH_ACK);
                    KbootDriverState.phase = KbootFlashPhase_ReceiveWriteResponse;
                    break;
                case KbootFlashPhase_ReceiveWriteResponse:
                    res.status = rx(KBOOT_PACKAGE_LENGTH_GENERIC_RESPONSE);
                    KbootDriverState.phase = KbootFlashPhase_SendWriteResponseAck;
                    break;
                case KbootFlashPhase_SendWriteResponseAck: {
                    if (rxBuffer[0] != 0x5a || rxBuffer[1] != 0xa4) {
                        KbootDriverState.phase = KbootFlashPhase_ReceiveWriteResponse;
                        res.status = kStatus_Uhk_IdleCycle;
                        res.hold = true;
                        break;
                    }
                    logResponsePacket("WriteMemory");
                    uint32_t status = getResponseStatus();
                    if (status != 0) {
                        LogU("Kboot: WriteMemory rejected (status=0x%x, addr=0x0, size=%u)\n",
                             status, KbootDriverState.firmwareSize);
                        abortFlash("WriteMemory command rejected");
                        break;
                    }
                    res.status = tx(ackMessage, sizeof(ackMessage));
                    ModuleFlashState = ModuleFlashState_Writing;
                    LogU("Kboot: Writing %u bytes...\n", KbootDriverState.firmwareSize);
                    KbootDriverState.phase = KbootFlashPhase_SendDataChunk;
                    break;
                }

                // Data chunk loop
                case KbootFlashPhase_SendDataChunk: {
                    uint32_t remaining = KbootDriverState.firmwareSize - KbootDriverState.firmwareOffset;
                    uint16_t chunkSize = remaining > KBOOT_DATA_CHUNK_SIZE ? KBOOT_DATA_CHUNK_SIZE : remaining;
                    uint8_t len = buildFramingPacket(0xa5,
                        KbootDriverState.firmwareData + KbootDriverState.firmwareOffset, chunkSize);
                    res.status = tx(txBuffer, len);
                    KbootDriverState.phase = KbootFlashPhase_ReceiveDataChunkAck;
                    break;
                }
                case KbootFlashPhase_ReceiveDataChunkAck: {
                    res.status = rx(KBOOT_PACKAGE_LENGTH_ACK);
                    uint32_t remaining = KbootDriverState.firmwareSize - KbootDriverState.firmwareOffset;
                    uint16_t chunkSize = remaining > KBOOT_DATA_CHUNK_SIZE ? KBOOT_DATA_CHUNK_SIZE : remaining;
                    KbootDriverState.firmwareOffset += chunkSize;
                    if (KbootDriverState.firmwareOffset % KBOOT_PROGRESS_LOG_BYTES < KBOOT_DATA_CHUNK_SIZE) {
                        LogU("Kboot: Write progress %u/%u bytes\n",
                             KbootDriverState.firmwareOffset, KbootDriverState.firmwareSize);
                    }
                    if (KbootDriverState.firmwareOffset >= KbootDriverState.firmwareSize) {
                        KbootDriverState.phase = KbootFlashPhase_ReceiveFinalResponse;
                    } else {
                        KbootDriverState.phase = KbootFlashPhase_SendDataChunk;
                    }
                    break;
                }

                // Final response after all data
                case KbootFlashPhase_ReceiveFinalResponse:
                    res.status = rx(KBOOT_PACKAGE_LENGTH_GENERIC_RESPONSE);
                    KbootDriverState.phase = KbootFlashPhase_SendFinalResponseAck;
                    break;
                case KbootFlashPhase_SendFinalResponseAck: {
                    if (rxBuffer[0] != 0x5a || rxBuffer[1] != 0xa4) {
                        KbootDriverState.phase = KbootFlashPhase_ReceiveFinalResponse;
                        res.status = kStatus_Uhk_IdleCycle;
                        res.hold = true;
                        break;
                    }
                    logResponsePacket("FinalWrite");
                    uint32_t status = getResponseStatus();
                    if (status != 0) {
                        LogU("Kboot: WriteMemory data failed (status=0x%x, offset=%u/%u)\n",
                             status, KbootDriverState.firmwareOffset, KbootDriverState.firmwareSize);
                        abortFlash("WriteMemory data transfer failed");
                        break;
                    }
                    res.status = tx(ackMessage, sizeof(ackMessage));
                    LogU("Kboot: Write complete, resetting module\n");
                    KbootDriverState.phase = KbootFlashPhase_SendReset;
                    break;
                }

                // Reset phases
                case KbootFlashPhase_SendReset:
                    LogU("Kboot: Sending reset to return to firmware\n");
                    res.status = tx(resetCommand, sizeof(resetCommand));
                    KbootDriverState.phase = KbootFlashPhase_ReceiveResetAck;
                    break;
                case KbootFlashPhase_ReceiveResetAck:
                    res.status = rx(KBOOT_PACKAGE_LENGTH_ACK);
                    KbootDriverState.phase = KbootFlashPhase_ReceiveResetGenericResponse;
                    break;
                case KbootFlashPhase_ReceiveResetGenericResponse:
                    res.status = rx(KBOOT_PACKAGE_LENGTH_GENERIC_RESPONSE);
                    KbootDriverState.phase = KbootFlashPhase_SendResetAck;
                    break;
                case KbootFlashPhase_SendResetAck:
                    res.status = tx(ackMessage, sizeof(ackMessage));
                    LogU("Kboot: Flash sequence complete (%dms total)\n", elapsedMs());
                    ModuleFlashState = ModuleFlashState_Done;
                    ModuleFlashBusy = false;
                    KbootDriverState.command = KbootCommand_Idle;
                    break;
            }
            break;
    }

    return res;
}
