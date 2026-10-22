#pragma once
#include <stdint.h>

#define MAG_HMC5883L_ADDR 0x1E
#define MAG_QMC5883L_ADDR 0x0D

typedef enum {
    MAG_NONE,
    MAG_HMC5883L,
    MAG_QMC5883L,   /* clone chip found on many GY-273 boards */
} MagType;

typedef struct {
    int16_t raw_x, raw_y, raw_z;   /* counts as read from the chip */
    int32_t x_mg, y_mg, z_mg;      /* milligauss */
    int32_t total_mg;              /* field magnitude, milligauss */
    int32_t heading_d10;           /* atan2(Y, X), tenths of a degree, 0-3599 */
    int     overflow;              /* 1 if any axis saturated */
} MagSample;

/* Detect the sensor (HMC5883L first, then QMC5883L), configure it and
 * check whether its DRDY output on PB5 works. Returns the detected type
 * or MAG_NONE. */
MagType     mag_init(void);

/* QMC5883L output data rate: 10, 50, 100 or 200 Hz (default 50). Applied at
 * once if the sensor is detected, otherwise on the next mag_init(). The
 * HMC5883L is read in single-measurement mode, so the rate does not apply
 * to it. Returns 0, or -1 for an unsupported rate or an I2C error. */
int         mag_set_rate(uint16_t hz);
uint16_t    mag_rate(void);

/* Type found by the last mag_init(); MAG_NONE after a failed read. */
MagType     mag_type(void);

/* 1 if DRDY (PB5) was seen on the last mag_init(). Without it the HMC5883L
 * status register is polled, and the QMC5883L is read without waiting. */
int         mag_drdy_ok(void);

const char *mag_name(MagType t);
uint8_t     mag_addr(MagType t);

/* Wait for a fresh sample (HMC5883L: start a single measurement) and read
 * it. Returns 0 on success, -1 on I2C error (the bus is
 * reset and the sensor must be detected again with mag_init()). */
int         mag_read(MagSample *s);
