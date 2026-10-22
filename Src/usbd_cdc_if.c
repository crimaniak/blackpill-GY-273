#include "usbd_cdc_if.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

static int8_t CDC_Init(void);
static int8_t CDC_DeInit(void);
static int8_t CDC_Control(uint8_t cmd, uint8_t *pbuf, uint16_t length);
static int8_t CDC_Receive(uint8_t *pbuf, uint32_t *Len);
static int8_t CDC_TransmitCplt(uint8_t *pbuf, uint32_t *Len, uint8_t epnum);

USBD_CDC_ItfTypeDef USBD_CDC_fops = {
    CDC_Init, CDC_DeInit, CDC_Control, CDC_Receive, CDC_TransmitCplt,
};

/* Created and initially given in main() before the scheduler starts */
QueueHandle_t     g_cdc_rx_queue  = NULL;
SemaphoreHandle_t g_cdc_tx_done   = NULL;
volatile uint8_t  g_cdc_connected = 0;

static uint8_t s_rx_buf[CDC_DATA_FS_MAX_PACKET_SIZE];

extern USBD_HandleTypeDef hUsbDeviceFS;

static int8_t CDC_Init(void) {
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, s_rx_buf);
    USBD_CDC_ReceivePacket(&hUsbDeviceFS);
    return USBD_OK;
}

static int8_t CDC_DeInit(void) {
    g_cdc_connected = 0;
    return USBD_OK;
}

static int8_t CDC_Control(uint8_t cmd, uint8_t *pbuf, uint16_t length) {
    (void)length;
    if (cmd == CDC_SET_CONTROL_LINE_STATE)
        g_cdc_connected = (pbuf[0] & 0x01U) ? 1U : 0U;
    return USBD_OK;
}

/* Called from USB IRQ handler — post each received byte to ConsoleTask */
static int8_t CDC_Receive(uint8_t *pbuf, uint32_t *Len) {
    BaseType_t higher = pdFALSE;
    if (g_cdc_rx_queue == NULL) {
        USBD_CDC_ReceivePacket(&hUsbDeviceFS);
        return USBD_OK;
    }
    for (uint32_t i = 0; i < *Len; i++)
        xQueueSendFromISR(g_cdc_rx_queue, &pbuf[i], &higher);
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, s_rx_buf);
    USBD_CDC_ReceivePacket(&hUsbDeviceFS);
    portYIELD_FROM_ISR(higher);
    return USBD_OK;
}

/* Called from USB IRQ handler when IN transfer finishes */
static int8_t CDC_TransmitCplt(uint8_t *pbuf, uint32_t *Len, uint8_t epnum) {
    (void)pbuf; (void)Len; (void)epnum;
    BaseType_t higher = pdFALSE;
    if (g_cdc_tx_done == NULL) return USBD_OK;
    xSemaphoreGiveFromISR(g_cdc_tx_done, &higher);
    portYIELD_FROM_ISR(higher);
    return USBD_OK;
}
