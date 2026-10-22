#include "i2c_bus.h"

#define SCL_PIN GPIO_PIN_6
#define SDA_PIN GPIO_PIN_7

I2C_HandleTypeDef g_i2c;

/* Crude busy-wait, roughly 1 us per unit at 96 MHz. Only used for bus
   recovery, where the exact timing does not matter. */
static void delay_us(uint32_t us) {
    volatile uint32_t n = us * 24;
    while (n--);
}

/* A reset in the middle of a read can leave a slave driving SDA low while
   it waits for the rest of its byte. Clock SCL by hand (up to 9 pulses)
   until it lets go, then put a STOP on the bus. */
static void bus_recover(void) {
    GPIO_InitTypeDef g = {
        .Pin   = SCL_PIN | SDA_PIN,
        .Mode  = GPIO_MODE_OUTPUT_OD,
        .Pull  = GPIO_PULLUP,
        .Speed = GPIO_SPEED_FREQ_LOW,
    };
    HAL_GPIO_WritePin(GPIOB, SCL_PIN | SDA_PIN, GPIO_PIN_SET);
    HAL_GPIO_Init(GPIOB, &g);
    delay_us(10);

    for (int i = 0; i < 9 && HAL_GPIO_ReadPin(GPIOB, SDA_PIN) == GPIO_PIN_RESET; i++) {
        HAL_GPIO_WritePin(GPIOB, SCL_PIN, GPIO_PIN_RESET);
        delay_us(5);
        HAL_GPIO_WritePin(GPIOB, SCL_PIN, GPIO_PIN_SET);
        delay_us(5);
    }

    /* STOP: SDA rises while SCL is high */
    HAL_GPIO_WritePin(GPIOB, SCL_PIN, GPIO_PIN_RESET);
    delay_us(5);
    HAL_GPIO_WritePin(GPIOB, SDA_PIN, GPIO_PIN_RESET);
    delay_us(5);
    HAL_GPIO_WritePin(GPIOB, SCL_PIN, GPIO_PIN_SET);
    delay_us(5);
    HAL_GPIO_WritePin(GPIOB, SDA_PIN, GPIO_PIN_SET);
    delay_us(5);
}

void i2c_bus_init(void) {
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    bus_recover();

    /* Peripheral reset clears a BUSY flag latched by a glitch on the bus */
    __HAL_RCC_I2C1_FORCE_RESET();
    __HAL_RCC_I2C1_RELEASE_RESET();

    /* The GY-273 has its own 4.7k pull-ups. The internal ones are enabled
       as well so that an empty bus idles high instead of floating. */
    GPIO_InitTypeDef g = {
        .Pin       = SCL_PIN | SDA_PIN,
        .Mode      = GPIO_MODE_AF_OD,
        .Pull      = GPIO_PULLUP,
        .Speed     = GPIO_SPEED_FREQ_HIGH,
        .Alternate = GPIO_AF4_I2C1,
    };
    HAL_GPIO_Init(GPIOB, &g);

    g_i2c.Instance             = I2C1;
    g_i2c.Init.ClockSpeed      = 100000;
    g_i2c.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    g_i2c.Init.OwnAddress1     = 0;
    g_i2c.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    g_i2c.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    g_i2c.Init.OwnAddress2     = 0;
    g_i2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    g_i2c.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    HAL_I2C_Init(&g_i2c);
}

int i2c_bus_scan(void (*print_fn)(const char *), uint8_t *found, int max) {
    static const char hex[] = "0123456789abcdef";
    char line[64];
    int  count = 0;

    print_fn("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
    for (int row = 0; row < 0x80; row += 16) {
        char *p = line;
        *p++ = hex[row >> 4];
        *p++ = '0';
        *p++ = ':';
        for (int col = 0; col < 16; col++) {
            int addr = row + col;
            *p++ = ' ';
            if (addr < 0x08 || addr > 0x77) {   /* reserved addresses */
                *p++ = ' ';
                *p++ = ' ';
                continue;
            }
            HAL_StatusTypeDef r =
                HAL_I2C_IsDeviceReady(&g_i2c, (uint16_t)(addr << 1), 1, 5);
            if (r == HAL_BUSY) {
                print_fn("\r\n");
                return -1;
            }
            if (r == HAL_OK) {
                *p++ = hex[addr >> 4];
                *p++ = hex[addr & 0xF];
                if (count < max) found[count] = (uint8_t)addr;
                count++;
            } else {
                *p++ = '-';
                *p++ = '-';
            }
        }
        *p++ = '\r';
        *p++ = '\n';
        *p   = '\0';
        print_fn(line);
    }
    return count;
}
