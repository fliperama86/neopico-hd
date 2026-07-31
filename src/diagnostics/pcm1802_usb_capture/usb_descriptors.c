#include <stddef.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

#define USB_VID 0xCAFE
#define USB_PID 0x4011
#define USB_BCD 0x0200

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&device_descriptor;
}

enum {
    INTERFACE_CDC_CONTROL = 0,
    INTERFACE_CDC_DATA,
    INTERFACE_COUNT,
};

#define ENDPOINT_CDC_NOTIFICATION 0x81
#define ENDPOINT_CDC_OUT 0x02
#define ENDPOINT_CDC_IN 0x82
#define CONFIGURATION_TOTAL_LENGTH (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, INTERFACE_COUNT, 0, CONFIGURATION_TOTAL_LENGTH, 0, 100),
    TUD_CDC_DESCRIPTOR(INTERFACE_CDC_CONTROL, 4, ENDPOINT_CDC_NOTIFICATION, 8, ENDPOINT_CDC_OUT, ENDPOINT_CDC_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return configuration_descriptor;
}

enum {
    STRING_LANGUAGE = 0,
    STRING_MANUFACTURER,
    STRING_PRODUCT,
    STRING_SERIAL,
    STRING_CDC_INTERFACE,
};

static const char *const string_descriptors[] = {
    (const char[]){0x09, 0x04}, "NeoPico-HD", "NeoPico PCM1802 Capture", NULL, "Framed PCM1802 Audio",
};

static uint16_t string_descriptor[33];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t language_id)
{
    (void)language_id;
    size_t character_count;

    if (index == STRING_LANGUAGE) {
        memcpy(&string_descriptor[1], string_descriptors[STRING_LANGUAGE], 2);
        character_count = 1;
    } else if (index == STRING_SERIAL) {
        character_count = board_usb_get_serial(&string_descriptor[1], 32);
    } else {
        if (index >= sizeof(string_descriptors) / sizeof(string_descriptors[0]) || string_descriptors[index] == NULL) {
            return NULL;
        }

        const char *string = string_descriptors[index];
        character_count = strlen(string);
        if (character_count > 32) {
            character_count = 32;
        }

        for (size_t i = 0; i < character_count; ++i) {
            string_descriptor[i + 1] = (uint8_t)string[i];
        }
    }

    string_descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * character_count + 2));
    return string_descriptor;
}
