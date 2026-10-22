#include "stm32f4xx_hal.h"
#include "usbd_def.h"
#include "usbd_core.h"
#include "usbd_conf.h"

PCD_HandleTypeDef hpcd;

/* ── MSP: clock + GPIO + NVIC ──────────────────────────────────────────── */
void HAL_PCD_MspInit(PCD_HandleTypeDef *hpcd_p) {
    (void)hpcd_p;

    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA11 = D-, PA12 = D+ (AF10, OTG_FS) */
    GPIO_InitTypeDef g = {
        .Pin       = GPIO_PIN_11 | GPIO_PIN_12,
        .Mode      = GPIO_MODE_AF_PP,
        .Pull      = GPIO_NOPULL,
        .Speed     = GPIO_SPEED_FREQ_VERY_HIGH,
        .Alternate = GPIO_AF10_OTG_FS,
    };
    HAL_GPIO_Init(GPIOA, &g);

    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();

    HAL_NVIC_SetPriority(OTG_FS_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(OTG_FS_IRQn);
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *hpcd_p) {
    (void)hpcd_p;
    __HAL_RCC_USB_OTG_FS_CLK_DISABLE();
}

/* ── HAL_PCD callbacks → USB Device Library ────────────────────────────── */
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd_p) {
    USBD_LL_SetupStage((USBD_HandleTypeDef *)hpcd_p->pData, (uint8_t *)hpcd_p->Setup);
}
void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd_p, uint8_t epnum) {
    USBD_LL_DataOutStage((USBD_HandleTypeDef *)hpcd_p->pData, epnum, hpcd_p->OUT_ep[epnum].xfer_buff);
}
void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd_p, uint8_t epnum) {
    USBD_LL_DataInStage((USBD_HandleTypeDef *)hpcd_p->pData, epnum, hpcd_p->IN_ep[epnum].xfer_buff);
}
void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd_p) {
    USBD_LL_SOF((USBD_HandleTypeDef *)hpcd_p->pData);
}
void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd_p) {
    USBD_LL_SetSpeed((USBD_HandleTypeDef *)hpcd_p->pData, USBD_SPEED_FULL);
    USBD_LL_Reset((USBD_HandleTypeDef *)hpcd_p->pData);
}
void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd_p) {
    USBD_LL_Suspend((USBD_HandleTypeDef *)hpcd_p->pData);
}
void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd_p) {
    USBD_LL_Resume((USBD_HandleTypeDef *)hpcd_p->pData);
}
void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd_p) {
    USBD_LL_DevConnected((USBD_HandleTypeDef *)hpcd_p->pData);
}
void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd_p) {
    USBD_LL_DevDisconnected((USBD_HandleTypeDef *)hpcd_p->pData);
}

/* ── USB Device Library → HAL_PCD interface ────────────────────────────── */
USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev) {
    hpcd.Instance                 = USB_OTG_FS;
    hpcd.Init.dev_endpoints       = 4;
    hpcd.Init.speed               = PCD_SPEED_FULL;
    hpcd.Init.dma_enable          = DISABLE;
    hpcd.Init.phy_itface          = PCD_PHY_EMBEDDED;
    hpcd.Init.Sof_enable          = DISABLE;
    hpcd.Init.low_power_enable    = DISABLE;
    hpcd.Init.lpm_enable          = DISABLE;
    hpcd.Init.vbus_sensing_enable = DISABLE;   /* VBUS (PA9) not wired on Black Pill */
    hpcd.Init.use_dedicated_ep1   = DISABLE;

    hpcd.pData = pdev;
    pdev->pData = &hpcd;

    HAL_PCD_Init(&hpcd);

    /* FIFO RAM is 320 words (1.25 KB) shared by RX and all TX FIFOs. */
    HAL_PCDEx_SetRxFiFo(&hpcd, 0x80);        /* shared RX FIFO: 128 words */
    HAL_PCDEx_SetTxFiFo(&hpcd, 0, 0x40);     /* EP0 IN:   64 words */
    HAL_PCDEx_SetTxFiFo(&hpcd, 1, 0x80);     /* EP1 IN (CDC data): 128 words */
    HAL_PCDEx_SetTxFiFo(&hpcd, 2, 0x20);     /* EP2 IN (CDC cmd):   32 words */

    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev) {
    HAL_PCD_DeInit((PCD_HandleTypeDef *)pdev->pData);
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev) {
    HAL_PCD_Start((PCD_HandleTypeDef *)pdev->pData);
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev) {
    HAL_PCD_Stop((PCD_HandleTypeDef *)pdev->pData);
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                   uint8_t ep_type, uint16_t ep_mps) {
    HAL_PCD_EP_Open((PCD_HandleTypeDef *)pdev->pData, ep_addr, ep_mps, ep_type);
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    HAL_PCD_EP_Close((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    HAL_PCD_EP_Flush((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    HAL_PCD_EP_SetStall((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    HAL_PCD_EP_ClrStall((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}
uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    PCD_HandleTypeDef *h = (PCD_HandleTypeDef *)pdev->pData;
    return (ep_addr & 0x80U) ? h->IN_ep[ep_addr & 0x7FU].is_stall
                             : h->OUT_ep[ep_addr & 0x7FU].is_stall;
}
USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr) {
    HAL_PCD_SetAddress((PCD_HandleTypeDef *)pdev->pData, dev_addr);
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                     uint8_t *pbuf, uint32_t size) {
    HAL_PCD_EP_Transmit((PCD_HandleTypeDef *)pdev->pData, ep_addr, pbuf, size);
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                           uint8_t *pbuf, uint32_t size) {
    HAL_PCD_EP_Receive((PCD_HandleTypeDef *)pdev->pData, ep_addr, pbuf, size);
    return USBD_OK;
}
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    return HAL_PCD_EP_GetRxCount((PCD_HandleTypeDef *)pdev->pData, ep_addr);
}
void USBD_LL_Delay(uint32_t Delay) {
    HAL_Delay(Delay);
}
void *USBD_static_malloc(uint32_t size) {
    static uint32_t mem[MAX_STATIC_ALLOC_SIZE / 4];
    (void)size;
    return mem;
}
void USBD_static_free(void *p) { (void)p; }
