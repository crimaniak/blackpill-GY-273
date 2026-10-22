#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_conf.h"

#define USBD_VID                    0x0483U
#define USBD_PID_CDC                0x5740U
#define USBD_LANGID_STRING          0x0409U
#define USBD_MANUFACTURER_STRING    "STMicroelectronics"
#define USBD_PRODUCT_STRING_CDC     "GY-273 Magnetometer Console"
#define USBD_SERIALNUMBER_STRING    "00000000001A"
#define USBD_CONFIGURATION_STRING   "CDC Config"
#define USBD_INTERFACE_STRING       "CDC Interface"

static void Get_SerialNum(void);
static void IntToUnicode(uint32_t value, uint8_t *pbuf, uint8_t len);

uint8_t *USBD_CDC_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_CDC_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_CDC_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_CDC_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_CDC_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_CDC_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_CDC_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);

USBD_DescriptorsTypeDef CDC_Desc = {
    USBD_CDC_DeviceDescriptor,
    USBD_CDC_LangIDStrDescriptor,
    USBD_CDC_ManufacturerStrDescriptor,
    USBD_CDC_ProductStrDescriptor,
    USBD_CDC_SerialStrDescriptor,
    USBD_CDC_ConfigStrDescriptor,
    USBD_CDC_InterfaceStrDescriptor,
};

static uint8_t hUSBDDeviceDesc[USB_LEN_DEV_DESC] = {
    0x12,          /* bLength */
    USB_DESC_TYPE_DEVICE,
    0x00, 0x02,    /* bcdUSB = 2.00 */
    0x02,          /* bDeviceClass: CDC */
    0x00,          /* bDeviceSubClass */
    0x00,          /* bDeviceProtocol */
    0x40,          /* bMaxPacketSize0 = 64 */
    LOBYTE(USBD_VID), HIBYTE(USBD_VID),
    LOBYTE(USBD_PID_CDC), HIBYTE(USBD_PID_CDC),
    0x00, 0x02,    /* bcdDevice = 2.00 */
    USBD_IDX_MFC_STR,
    USBD_IDX_PRODUCT_STR,
    USBD_IDX_SERIAL_STR,
    USBD_MAX_NUM_CONFIGURATION
};

static uint8_t USBD_LangIDDesc[USB_LEN_LANGID_STR_DESC] = {
    USB_LEN_LANGID_STR_DESC,
    USB_DESC_TYPE_STRING,
    LOBYTE(USBD_LANGID_STRING),
    HIBYTE(USBD_LANGID_STRING),
};

static uint8_t USBD_StrDesc[USBD_MAX_STR_DESC_SIZ];
static uint8_t USBD_StringSerial[0x1AU] = {
    0x1A, USB_DESC_TYPE_STRING,
};

uint8_t *USBD_CDC_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    *length = sizeof(hUSBDDeviceDesc);
    return hUSBDDeviceDesc;
}

uint8_t *USBD_CDC_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    *length = sizeof(USBD_LangIDDesc);
    return USBD_LangIDDesc;
}

uint8_t *USBD_CDC_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    USBD_GetString((uint8_t *)USBD_PRODUCT_STRING_CDC, USBD_StrDesc, length);
    return USBD_StrDesc;
}

uint8_t *USBD_CDC_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    USBD_GetString((uint8_t *)USBD_MANUFACTURER_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

uint8_t *USBD_CDC_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    *length = 0x1A;
    Get_SerialNum();
    return USBD_StringSerial;
}

uint8_t *USBD_CDC_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    USBD_GetString((uint8_t *)USBD_CONFIGURATION_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

uint8_t *USBD_CDC_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    USBD_GetString((uint8_t *)USBD_INTERFACE_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

static void Get_SerialNum(void) {
    uint32_t id0 = *(uint32_t *)0x1FFFF7E8U;
    uint32_t id1 = *(uint32_t *)0x1FFFF7ECU;
    uint32_t id2 = *(uint32_t *)0x1FFFF7F0U;
    uint32_t serial0 = id0 + id2;
    uint32_t serial1 = id1;
    IntToUnicode(serial0, &USBD_StringSerial[2], 8);
    IntToUnicode(serial1, &USBD_StringSerial[18], 4);
}

static void IntToUnicode(uint32_t value, uint8_t *pbuf, uint8_t len) {
    for (uint8_t idx = 0; idx < len; idx++) {
        uint8_t nibble = (value >> 28) & 0x0FU;
        pbuf[2 * idx]     = (nibble < 10U) ? (nibble + '0') : (nibble + 'A' - 10U);
        pbuf[2 * idx + 1] = 0;
        value <<= 4;
    }
}
