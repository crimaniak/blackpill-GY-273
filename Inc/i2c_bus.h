#pragma once
#include <stdint.h>
#include "stm32f4xx_hal.h"

/* I2C1 on PB6 (SCL) / PB7 (SDA), 100 kHz */
extern I2C_HandleTypeDef g_i2c;

/* Configure (or reconfigure) I2C1. Frees a slave stuck holding SDA low and
 * resets the peripheral, so it is also the recovery path after bus errors. */
void i2c_bus_init(void);

/* Probe addresses 0x08-0x77 and print an i2cdetect-style map via print_fn.
 * Addresses that ACK are stored in found[] (up to max entries).
 * Returns the number of devices found, or -1 if the bus is stuck busy. */
int  i2c_bus_scan(void (*print_fn)(const char *), uint8_t *found, int max);
