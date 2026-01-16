#include <string.h>
#include "usb_protocol_handler.h"
#include "usb_commands/usb_command_set_uhk60_led_state.h"
#include "led_manager.h"
#include "led_display.h"

void UsbCommand_SetUhk60LedState(const uint8_t *GenericHidOutBuffer, uint8_t *GenericHidInBuffer)
{
    memcpy((uint8_t*)&Uhk60LedState, GenericHidOutBuffer + 1, sizeof(uhk60_led_state_t));
    LedDisplay_UpdateAll();
}
