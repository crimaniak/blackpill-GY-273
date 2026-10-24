#include "display.h"
#include "fmt.h"
#include "mag_compass.h"
#include "ssd1306.h"
#include "ssd1306_fonts.h"

/* Used by the ssd1306 driver as SSD1306_SPI_PORT */
SPI_HandleTypeDef hspi1;

/*
 * Layout (128 x 64):
 *    0  234.1o SW      heading, 16x26 font, and the nearest compass point
 *   30  |B| 470 mG  CAL
 *   43  X -236 Y -327 Z  241   (mG)
 *   55  QMC5883L 50Hz DRDY
 */

#define SCK_PIN   GPIO_PIN_5
#define MOSI_PIN  GPIO_PIN_7

int display_detect(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef g = {
        .Pin   = SCK_PIN | MOSI_PIN,
        .Mode  = GPIO_MODE_INPUT,
        .Pull  = GPIO_PULLDOWN,   /* ~40 kOhm, weaker than the module's pull-ups */
        .Speed = GPIO_SPEED_FREQ_LOW,
    };
    HAL_GPIO_Init(GPIOA, &g);
    HAL_Delay(1);   /* let the lines settle */
    return HAL_GPIO_ReadPin(GPIOA, SCK_PIN) == GPIO_PIN_SET &&
           HAL_GPIO_ReadPin(GPIOA, MOSI_PIN) == GPIO_PIN_SET;
}

void display_init(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    GPIO_InitTypeDef g = {
        .Pin       = SCK_PIN | MOSI_PIN,
        .Mode      = GPIO_MODE_AF_PP,
        .Pull      = GPIO_NOPULL,
        .Speed     = GPIO_SPEED_FREQ_HIGH,
        .Alternate = GPIO_AF5_SPI1,
    };
    HAL_GPIO_Init(GPIOA, &g);

    /* Control lines idle high before they become outputs */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);                 /* CS */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_1, GPIO_PIN_SET);    /* DC, RES */
    g = (GPIO_InitTypeDef){
        .Pin   = GPIO_PIN_4,
        .Mode  = GPIO_MODE_OUTPUT_PP,
        .Pull  = GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_HIGH,
    };
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_0 | GPIO_PIN_1;
    HAL_GPIO_Init(GPIOB, &g);

    /* Mode 0, MSB first. APB2 96 MHz / 16 = 6 MHz (the controller allows 10) */
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial     = 7;
    HAL_SPI_Init(&hspi1);

    ssd1306_Init();   /* pulses RES, sends the init sequence, clears */
}

static void text(uint8_t x, uint8_t y, const char *s, SSD1306_Font_t font) {
    ssd1306_SetCursor(x, y);
    ssd1306_WriteString((char *)s, font, White);
}

/* Nearest of the 8 compass points */
static const char *compass_point(float heading) {
    static const char *const points[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int i = (int)((heading + 22.5f) / 45.0f);
    return points[i & 7];
}

void display_reading(const mag_sample_t *s, const mag_dev_t *dev) {
    char buf[32];
    char *p;
    float heading = mag_heading_deg(s, 0.0f);
    int32_t h10 = round_i32(heading * 10.0f) % 3600;

    ssd1306_Fill(Black);

    /* "234.1", the degree sign as a small circle, compass point */
    p = fmt_int(buf, h10 / 10, 3);
    *p++ = '.';
    *p++ = (char)('0' + h10 % 10);
    *p = '\0';
    text(0, 0, buf, Font_16x26);
    ssd1306_DrawCircle(84, 3, 2, White);
    text(95, 4, compass_point(heading), Font_11x18);

    /* 1 uT = 10 mG */
    p = fmt_str(buf, "|B| ");
    p = fmt_int(p, round_i32(mag_field_ut(s) * 10.0f), 0);
    p = fmt_str(p, " mG");
    *p = '\0';
    text(0, 30, buf, Font_7x10);
    text(107, 30, (s->flags & MAG_FLAG_CALIBRATED) ? "CAL" : "RAW", Font_7x10);

    p = fmt_str(buf, "X");
    p = fmt_int(p, round_i32(s->x * 10.0f), 5);
    p = fmt_str(p, " Y");
    p = fmt_int(p, round_i32(s->y * 10.0f), 5);
    p = fmt_str(p, " Z");
    p = fmt_int(p, round_i32(s->z * 10.0f), 5);
    *p = '\0';
    text(0, 43, buf, Font_6x8);

    if (s->flags & MAG_FLAG_OVERFLOW) {
        text(0, 55, "OVERFLOW: field too strong", Font_6x8);
    } else {
        p = fmt_str(buf, mag_chip_name(dev));
        *p++ = ' ';
        p = fmt_dec2(p, dev->cfg.rate_hz);
        p = fmt_str(p, dev->drdy_ok ? "Hz DRDY" : "Hz no DRDY");
        *p = '\0';
        text(0, 55, buf, Font_6x8);
    }

    ssd1306_UpdateScreen();
}

void display_calibration(const int cover[3], int secs_left) {
    static const char *const axes[] = {"X", "Y", "Z"};
    char buf[24];
    char *p;

    ssd1306_Fill(Black);
    text(0, 0, "Calibrating", Font_11x18);

    /* One bar per axis: how much of a full turn it has seen */
    for (int i = 0; i < 3; i++) {
        uint8_t y = (uint8_t)(21 + i * 11);
        int c = cover[i] < 0 ? 0 : cover[i] > 100 ? 100 : cover[i];
        text(0, y, axes[i], Font_7x10);
        ssd1306_DrawRectangle(10, y, 100, y + 8, White);
        if (c > 0)
            ssd1306_FillRectangle(10, y, (uint8_t)(10 + 90 * c / 100), y + 8, White);
        p = fmt_int(buf, c, 3);
        *p++ = '%';
        *p = '\0';
        text(104, y, buf, Font_6x8);
    }

    p = fmt_int(buf, secs_left, 0);
    p = fmt_str(p, " s left, keep turning");
    *p = '\0';
    text(0, 56, buf, Font_6x8);

    ssd1306_UpdateScreen();
}

void display_message(const char *title, const char *detail) {
    ssd1306_Fill(Black);
    text(0, 10, title, Font_11x18);
    text(0, 40, detail, Font_6x8);
    ssd1306_UpdateScreen();
}
