/* Configuration for the stm32-ssd1306 driver (Lib/stm32-ssd1306).
   0.96" 128x64 OLED (SSD1306 / SSD1315) on SPI1. */
#ifndef __SSD1306_CONF_H__
#define __SSD1306_CONF_H__

#define STM32F4
#define SSD1306_USE_SPI

#define SSD1306_SPI_PORT    hspi1       /* defined in display.c */
#define SSD1306_CS_Port     GPIOA
#define SSD1306_CS_Pin      GPIO_PIN_4
#define SSD1306_DC_Port     GPIOB
#define SSD1306_DC_Pin      GPIO_PIN_0
#define SSD1306_Reset_Port  GPIOB
#define SSD1306_Reset_Pin   GPIO_PIN_1

#define SSD1306_INCLUDE_FONT_6x8
#define SSD1306_INCLUDE_FONT_7x10
#define SSD1306_INCLUDE_FONT_11x18
#define SSD1306_INCLUDE_FONT_16x26

#endif
