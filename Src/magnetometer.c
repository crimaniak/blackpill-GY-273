#include "magnetometer.h"
#include "i2c_bus.h"

#define I2C_TIMEOUT_MS   20
#define READY_TIMEOUT_MS 100

/* DRDY output of the module */
#define DRDY_PORT GPIOB
#define DRDY_PIN  GPIO_PIN_5

/* HMC5883L registers */
#define HMC_CRA            0x00
#define HMC_CRB            0x01
#define HMC_MODE           0x02
#define HMC_MODE_SINGLE    0x01
#define HMC_DATA           0x03   /* X, Z, Y; big-endian */
#define HMC_STATUS         0x09
#define HMC_STATUS_RDY     0x01
#define HMC_ID             0x0A   /* "H43" */
#define HMC_LSB_PER_GAUSS  1090   /* gain 1, +-1.3 Ga */
#define HMC_OVERFLOW       (-4096)

/* QMC5883L registers */
#define QMC_DATA           0x00   /* X, Y, Z; little-endian; then STATUS */
#define QMC_STATUS_OVL     0x02
#define QMC_STATUS_BAD     0xF8   /* reserved bits, always 0 in a real read */
#define QMC_READ_TRIES     4
#define QMC_CTRL1          0x09
#define QMC_CTRL2          0x0A
#define QMC_SET_RESET      0x0B
#define QMC_ID             0x0D   /* reads 0xFF */
#define QMC_LSB_PER_GAUSS  12000  /* +-2 G range */

static MagType s_type = MAG_NONE;
static int     s_drdy_ok;   /* DRDY line seen working by mag_init() */

/* After a timeout the I2C peripheral is left with BUSY set and every later
   transfer fails until the bus is reset, so reset it right away. A NACK
   needs nothing: the HAL ends that transfer with a STOP. */
static int check(HAL_StatusTypeDef r) {
    if (r == HAL_OK)
        return 0;
    if (r == HAL_BUSY || (g_i2c.ErrorCode & HAL_I2C_ERROR_TIMEOUT))
        i2c_bus_init();
    return -1;
}

static int rd(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t n) {
    return check(HAL_I2C_Mem_Read(&g_i2c, (uint16_t)(addr << 1), reg,
                                  I2C_MEMADD_SIZE_8BIT, buf, n, I2C_TIMEOUT_MS));
}

static int wr(uint8_t addr, uint8_t reg, uint8_t val) {
    return check(HAL_I2C_Mem_Write(&g_i2c, (uint16_t)(addr << 1), reg,
                                   I2C_MEMADD_SIZE_8BIT, &val, 1, I2C_TIMEOUT_MS));
}

static int present(uint8_t addr) {
    return HAL_I2C_IsDeviceReady(&g_i2c, (uint16_t)(addr << 1), 2, 5) == HAL_OK;
}

static int hmc_setup(void) {
    uint8_t id[3];
    if (rd(MAG_HMC5883L_ADDR, HMC_ID, id, 3) != 0 ||
        id[0] != 'H' || id[1] != '4' || id[2] != '3')
        return -1;
    /* CRA: 8-sample average, no bias. CRB: gain 1. Each reading is a single
       measurement started by hmc_trigger(). */
    if (wr(MAG_HMC5883L_ADDR, HMC_CRA, 0x70) != 0 ||
        wr(MAG_HMC5883L_ADDR, HMC_CRB, 0x20) != 0)
        return -1;
    return 0;
}

static int qmc_setup(void) {
    uint8_t id;
    if (rd(MAG_QMC5883L_ADDR, QMC_ID, &id, 1) != 0 || id != 0xFF)
        return -1;
    /* Soft reset, SET/RESET period 0x01 as the datasheet recommends, then
       CTRL1: OSR 512, +-2 G, 50 Hz, continuous. A chip that was already
       measuring takes more than 5 ms to accept writes after the reset, so
       retry for up to 50 ms and confirm by reading CTRL1 back. */
    if (wr(MAG_QMC5883L_ADDR, QMC_CTRL2, 0x80) != 0)
        return -1;
    for (int i = 0; i < 25; i++) {
        HAL_Delay(2);
        uint8_t ctrl1;
        if (wr(MAG_QMC5883L_ADDR, QMC_SET_RESET, 0x01) == 0 &&
            wr(MAG_QMC5883L_ADDR, QMC_CTRL1, 0x05) == 0 &&
            rd(MAG_QMC5883L_ADDR, QMC_CTRL1, &ctrl1, 1) == 0 && ctrl1 == 0x05)
            return 0;
    }
    return -1;
}

/* DRDY is polled through the EXTI pending flag (the NVIC line stays
   disabled), so the 250 us low pulse of the HMC5883L cannot be missed.
   The QMC5883L holds DRDY high until the data registers are read. */
static void drdy_config(MagType t) {
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    GPIO_InitTypeDef g = {
        .Pin   = DRDY_PIN,
        .Mode  = t == MAG_HMC5883L ? GPIO_MODE_IT_FALLING : GPIO_MODE_IT_RISING,
        .Pull  = t == MAG_HMC5883L ? GPIO_PULLUP : GPIO_PULLDOWN,
        .Speed = GPIO_SPEED_FREQ_LOW,
    };
    HAL_GPIO_Init(DRDY_PORT, &g);
    __HAL_GPIO_EXTI_CLEAR_FLAG(DRDY_PIN);
}

static int drdy_active(void) {
    if (__HAL_GPIO_EXTI_GET_FLAG(DRDY_PIN))
        return 1;
    return s_type == MAG_QMC5883L && HAL_GPIO_ReadPin(DRDY_PORT, DRDY_PIN) == GPIO_PIN_SET;
}

/* Wait for a fresh sample: on the DRDY line if it works, otherwise by
   polling the status register. Returns 0 when one is ready.
   The QMC5883L clone tested here now and then hangs the bus when its status
   register is polled, so without DRDY it is not polled: in continuous mode
   its data registers always hold the latest sample. */
static int wait_ready(void) {
    if (!s_drdy_ok && s_type == MAG_QMC5883L)
        return 0;

    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < READY_TIMEOUT_MS) {
        if (s_drdy_ok) {
            if (drdy_active()) return 0;
        } else {
            uint8_t st;
            if (rd(MAG_HMC5883L_ADDR, HMC_STATUS, &st, 1) == 0 && (st & HMC_STATUS_RDY))
                return 0;
            HAL_Delay(1);
        }
    }
    return -1;
}

/* Start a single measurement on the HMC5883L. */
static int hmc_trigger(void) {
    __HAL_GPIO_EXTI_CLEAR_FLAG(DRDY_PIN);
    return wr(MAG_HMC5883L_ADDR, HMC_MODE, HMC_MODE_SINGLE);
}

MagType mag_init(void) {
    s_type = MAG_NONE;
    if (__HAL_I2C_GET_FLAG(&g_i2c, I2C_FLAG_BUSY))
        i2c_bus_init();

    MagType t = MAG_NONE;
    if (present(MAG_HMC5883L_ADDR) && hmc_setup() == 0)
        t = MAG_HMC5883L;
    else if (present(MAG_QMC5883L_ADDR) && qmc_setup() == 0)
        t = MAG_QMC5883L;
    if (t == MAG_NONE)
        return MAG_NONE;

    /* Check the DRDY line on the first conversion */
    s_type = t;
    drdy_config(t);
    if (t == MAG_HMC5883L && hmc_trigger() != 0) {
        s_type = MAG_NONE;
        return MAG_NONE;
    }
    s_drdy_ok = 1;
    if (wait_ready() != 0) {
        s_drdy_ok = 0;
        if (wait_ready() != 0) {   /* no data at all */
            s_type = MAG_NONE;
            return MAG_NONE;
        }
    }
    return s_type;
}

int mag_drdy_ok(void) {
    return s_drdy_ok;
}

MagType mag_type(void) {
    return s_type;
}

const char *mag_name(MagType t) {
    switch (t) {
    case MAG_HMC5883L: return "HMC5883L";
    case MAG_QMC5883L: return "QMC5883L";
    default:           return "none";
    }
}

uint8_t mag_addr(MagType t) {
    switch (t) {
    case MAG_HMC5883L: return MAG_HMC5883L_ADDR;
    case MAG_QMC5883L: return MAG_QMC5883L_ADDR;
    default:           return 0;
    }
}

/* atan2 in degrees, -180..180, max error about 0.3 degrees.
   The build has no libm, so use atan(z) ~ z*pi/4 + 0.273*z*(1-z) on 0..1. */
static float atan2_deg(float y, float x) {
    float ax = x < 0 ? -x : x;
    float ay = y < 0 ? -y : y;
    if (ax == 0.0f && ay == 0.0f) return 0.0f;
    float z = ax >= ay ? ay / ax : ax / ay;
    float a = z * (45.0f + 15.642f * (1.0f - z));
    if (ay > ax) a = 90.0f - a;
    if (x < 0)   a = 180.0f - a;
    if (y < 0)   a = -a;
    return a;
}

/* The QMC5883L clone tested here answers the first burst read after each
   new sample with only the first byte, followed by 0xFF. The burst after
   that returns the whole sample. A failed burst is detected by the status
   byte, whose reserved bits read as 1. */
static int qmc_read_data(uint8_t *b) {
    for (int i = 0; i < QMC_READ_TRIES; i++) {
        if (rd(MAG_QMC5883L_ADDR, QMC_DATA, b, 7) != 0)
            HAL_Delay(1);   /* occasional NACK: give the chip a moment */
        else if (!(b[6] & QMC_STATUS_BAD))
            return 0;
    }
    return -1;
}

int mag_read(MagSample *s) {
    uint8_t b[7];
    int32_t lsb;

    switch (s_type) {
    case MAG_HMC5883L:
        if (hmc_trigger() != 0 || wait_ready() != 0 ||
            rd(MAG_HMC5883L_ADDR, HMC_DATA, b, 6) != 0)
            goto fail;
        s->raw_x = (int16_t)(b[0] << 8 | b[1]);
        s->raw_z = (int16_t)(b[2] << 8 | b[3]);
        s->raw_y = (int16_t)(b[4] << 8 | b[5]);
        s->overflow = s->raw_x == HMC_OVERFLOW || s->raw_y == HMC_OVERFLOW ||
                      s->raw_z == HMC_OVERFLOW;
        lsb = HMC_LSB_PER_GAUSS;
        break;
    case MAG_QMC5883L:
        __HAL_GPIO_EXTI_CLEAR_FLAG(DRDY_PIN);
        if (wait_ready() != 0 || qmc_read_data(b) != 0)
            goto fail;
        s->raw_x = (int16_t)(b[1] << 8 | b[0]);
        s->raw_y = (int16_t)(b[3] << 8 | b[2]);
        s->raw_z = (int16_t)(b[5] << 8 | b[4]);
        s->overflow = (b[6] & QMC_STATUS_OVL) != 0;
        lsb = QMC_LSB_PER_GAUSS;
        break;
    default:
        return -1;
    }

    s->x_mg = (int32_t)s->raw_x * 1000 / lsb;
    s->y_mg = (int32_t)s->raw_y * 1000 / lsb;
    s->z_mg = (int32_t)s->raw_z * 1000 / lsb;

    float fx = (float)s->raw_x, fy = (float)s->raw_y, fz = (float)s->raw_z;
    s->total_mg = (int32_t)(__builtin_sqrtf(fx * fx + fy * fy + fz * fz) * 1000.0f / (float)lsb + 0.5f);

    float h = atan2_deg(fy, fx);
    if (h < 0) h += 360.0f;
    s->heading_d10 = (int32_t)(h * 10.0f + 0.5f) % 3600;
    return 0;

fail:
    s_type = MAG_NONE;
    i2c_bus_init();
    return -1;
}
