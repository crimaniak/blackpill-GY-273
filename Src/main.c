#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"
#include "console.h"
#include "i2c_bus.h"

USBD_HandleTypeDef hUsbDeviceFS;

static void SystemClock_Config(void);

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask; (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    for (;;);
}

int main(void) {
    HAL_Init();
    SystemClock_Config();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* D+ re-enumeration: pull PA12 low so the host sees a disconnect */
    GPIO_InitTypeDef g = {
        .Pin   = GPIO_PIN_12,
        .Mode  = GPIO_MODE_OUTPUT_PP,
        .Speed = GPIO_SPEED_FREQ_HIGH,
    };
    HAL_GPIO_Init(GPIOA, &g);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_12);

    i2c_bus_init();

    USBD_Init(&hUsbDeviceFS, &CDC_Desc, 0);
    USBD_RegisterClass(&hUsbDeviceFS, &USBD_CDC);
    USBD_CDC_RegisterInterface(&hUsbDeviceFS, &USBD_CDC_fops);
    USBD_Start(&hUsbDeviceFS);

    /* Create FreeRTOS primitives after USB init: HAL_Delay() inside the USB
       start-up needs SysTick, and a critical section entered before the
       scheduler starts (xSemaphoreGive) leaves BASEPRI raised, which masks
       SysTick and would hang HAL_Delay(). The CDC callbacks ignore events
       until the objects exist. */
    g_cdc_rx_queue = xQueueCreate(256, sizeof(uint8_t));
    g_cdc_tx_done  = xSemaphoreCreateBinary();
    xSemaphoreGive(g_cdc_tx_done);   /* mark as initially available */

    /* Only ConsoleTask needed — USB is fully interrupt-driven */
    xTaskCreate(ConsoleTask, "CON", 768, NULL, 2, NULL);

    vTaskStartScheduler();
    for (;;);
}

static void SystemClock_Config(void) {
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    /* HSE 25 MHz / 25 * 192 = 192 MHz VCO; /2 = 96 MHz SYSCLK; /4 = 48 MHz USB */
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM       = 25;
    osc.PLL.PLLN       = 192;
    osc.PLL.PLLP       = RCC_PLLP_DIV2;
    osc.PLL.PLLQ       = 4;
    HAL_RCC_OscConfig(&osc);

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType      = RCC_CLOCKTYPE_HCLK  | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;   /* 48 MHz (max 50) */
    clk.APB2CLKDivider = RCC_HCLK_DIV1;   /* 96 MHz (max 100) */
    HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_3);
}
