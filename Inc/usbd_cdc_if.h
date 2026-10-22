#pragma once
#include "usbd_cdc.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

extern USBD_CDC_ItfTypeDef USBD_CDC_fops;

/* Byte queue fed by the CDC Receive callback (ISR context) */
extern QueueHandle_t     g_cdc_rx_queue;

/* Binary semaphore: given by TransmitCplt, taken by cdc_write before
   starting a new transmit.  Created and given once in main(). */
extern SemaphoreHandle_t g_cdc_tx_done;

/* Set to 1 when host asserts DTR (terminal opened), 0 otherwise */
extern volatile uint8_t  g_cdc_connected;
