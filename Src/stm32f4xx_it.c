#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

extern PCD_HandleTypeDef hpcd;
extern void xPortSysTickHandler(void);

void SysTick_Handler(void) {
    HAL_IncTick();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        xPortSysTickHandler();
}

void OTG_FS_IRQHandler(void) { HAL_PCD_IRQHandler(&hpcd); }

void NMI_Handler(void)         { for (;;); }
void HardFault_Handler(void)   { for (;;); }
void MemManage_Handler(void)   { for (;;); }
void BusFault_Handler(void)    { for (;;); }
void UsageFault_Handler(void)  { for (;;); }
