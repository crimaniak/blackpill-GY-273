#pragma once
#include "i2c_magnetometer.h"

/* 0.96" 128x64 OLED (SSD1306 / SSD1315) on SPI1:
   SCK PA5, MOSI PA7, CS PA4, DC PB0, RES PB1. */

/* The display cannot be read back over SPI. Instead this checks for the
   pull-up resistors the module has on D0 and D1 (for its I2C mode): with
   the internal pull-downs, both read high only when a module is attached.
   Leaves the pins as inputs. Call before display_init(). */
int  display_detect(void);

/* Set up SPI1 and the controller. Only when display_detect() found one. */
void display_init(void);

/* Heading, field and sensor status of one reading */
void display_reading(const mag_sample_t *s, const mag_dev_t *dev);

/* Calibration progress: coverage per axis in %, seconds left */
void display_calibration(const int cover[3], int secs_left);

/* A title and a smaller second line, e.g. "No sensor" / "check wiring" */
void display_message(const char *title, const char *detail);
