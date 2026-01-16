#ifndef __LED_MANAGER_H__
#define __LED_MANAGER_H__

// Includes:

    #include <stdbool.h>
    #include <stdint.h>
    #include "attributes.h"

// Macros:

    #define LED_OVERRIDE_KEY_COUNT 256
    #define LED_OVERRIDE_KEY_BYTES (LED_OVERRIDE_KEY_COUNT / 8)

// Typedefs:

    typedef struct {
        uint8_t mod : 1;
        uint8_t fn : 1;
        uint8_t mouse : 1;
        uint8_t capsLock : 1;
        uint8_t agent : 1;
        uint8_t adaptive : 1;
        uint8_t segmentDisplay : 1;
        uint8_t reserved : 1;
    } ATTR_PACKED led_override_uhk60_t;

    typedef struct {
        led_override_uhk60_t uhk60Leds;                         // 1 byte
        uint8_t oledOverride;                                   // 1 byte
        uint8_t keyBacklightOverrides[LED_OVERRIDE_KEY_BYTES];  // 32 bytes
    } ATTR_PACKED led_override_t;

// Variables:

    extern led_override_t LedOverride;

    extern uint8_t DisplayBrightness;
    extern uint8_t KeyBacklightBrightness;

    extern bool KeyBacklightSleepModeActive;
    extern bool DisplaySleepModeActive;

    extern bool AlwaysOnMode;

// Functions:

    void LedManager_FullUpdate();
    void LedManager_RecalculateLedBrightness();
    void LedManager_UpdateAgentLed();
    void LedManager_UpdateSleepModes();


#endif
