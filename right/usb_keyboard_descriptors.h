#ifndef __USB_KEYBOARD_DESCRIPTORS_H__
#define __USB_KEYBOARD_DESCRIPTORS_H__

// Macros:

    #define USB_KEYBOARD_CLASS (0x03U)
    #define USB_KEYBOARD_SUBCLASS (0x01U)
    #define USB_KEYBOARD_PROTOCOL (0x01U)

    #define USB_KEYBOARD_INTERFACE_INDEX (1U)
    #define USB_KEYBOARD_INTERFACE_COUNT (1U)
    #define USB_KEYBOARD_INTERFACE_ALTERNATE_SETTING (0U)

    #define USB_KEYBOARD_ENDPOINT_IN (2U)
    #define USB_KEYBOARD_ENDPOINT_COUNT (1U)

    #define USB_KEYBOARD_INTERRUPT_IN_PACKET_SIZE (8U)
    #define USB_KEYBOARD_INTERRUPT_IN_INTERVAL (0x04U)

    #define USB_KEYBOARD_REPORT_LENGTH (0x08U)
    #define USB_KEYBOARD_REPORT_DESCRIPTOR_LENGTH (63U)
    #define USB_KEYBOARD_STRING_DESCRIPTOR_LENGTH (40U)

// Variables:

    extern usb_device_class_struct_t UsbKeyboardClass;
    extern uint8_t UsbKeyboardReportDescriptor[USB_KEYBOARD_REPORT_DESCRIPTOR_LENGTH];
    extern uint8_t UsbKeyboardString[USB_KEYBOARD_STRING_DESCRIPTOR_LENGTH];

#endif
