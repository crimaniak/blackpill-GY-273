# Black Pill (STM32F411CEU6) GY-273 magnetometer console
ROOT     := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))

# All dependencies are git submodules: git submodule update --init
CMSIS      := $(ROOT)/Drivers/cmsis-device-f4
CMSIS_CORE := $(ROOT)/Drivers/cmsis-core/Include
HAL_DRV    := $(ROOT)/Drivers/stm32f4xx-hal-driver
FREERTOS   := $(ROOT)/Middlewares/FreeRTOS
USB_LIB    := $(ROOT)/Middlewares/stm32_mw_usb_device

CC       := arm-none-eabi-gcc
OBJCOPY  := arm-none-eabi-objcopy

INCLUDES := -I$(ROOT)/Inc \
            -I$(FREERTOS)/include \
            -I$(FREERTOS)/portable/GCC/ARM_CM4F \
            -I$(USB_LIB)/Core/Inc \
            -I$(USB_LIB)/Class/CDC/Inc \
            -I$(CMSIS)/Include \
            -I$(CMSIS_CORE) \
            -I$(HAL_DRV)/Inc

DEFS     := -DSTM32F411xE -DUSE_HAL_DRIVER

ARCH     := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CFLAGS   := $(ARCH) -O2 -ffreestanding -fno-math-errno \
            -ffunction-sections -fdata-sections \
            $(INCLUDES) $(DEFS)

LDSCRIPT := $(ROOT)/STM32F411CE_FLASH.ld
LDFLAGS  := $(ARCH) -T$(LDSCRIPT) -nostdlib \
            -Wl,--gc-sections -lgcc

BUILD    := $(ROOT)/build

PROJ_SRCS := $(ROOT)/Src/main.c \
             $(ROOT)/Src/stm32f4xx_it.c \
             $(ROOT)/Src/syscalls.c \
             $(ROOT)/Src/string_stubs.c \
             $(ROOT)/Src/usbd_conf.c \
             $(ROOT)/Src/usbd_desc.c \
             $(ROOT)/Src/usbd_cdc_if.c \
             $(ROOT)/Src/console.c \
             $(ROOT)/Src/i2c_bus.c \
             $(ROOT)/Src/magnetometer.c

HAL_SRCS := $(HAL_DRV)/Src/stm32f4xx_hal.c \
            $(HAL_DRV)/Src/stm32f4xx_hal_cortex.c \
            $(HAL_DRV)/Src/stm32f4xx_hal_dma.c \
            $(HAL_DRV)/Src/stm32f4xx_hal_flash.c \
            $(HAL_DRV)/Src/stm32f4xx_hal_gpio.c \
            $(HAL_DRV)/Src/stm32f4xx_hal_i2c.c \
            $(HAL_DRV)/Src/stm32f4xx_hal_pcd.c \
            $(HAL_DRV)/Src/stm32f4xx_hal_pcd_ex.c \
            $(HAL_DRV)/Src/stm32f4xx_hal_pwr.c \
            $(HAL_DRV)/Src/stm32f4xx_hal_pwr_ex.c \
            $(HAL_DRV)/Src/stm32f4xx_hal_rcc.c \
            $(HAL_DRV)/Src/stm32f4xx_hal_rcc_ex.c \
            $(HAL_DRV)/Src/stm32f4xx_ll_usb.c

RTOS_SRCS := $(FREERTOS)/tasks.c \
             $(FREERTOS)/queue.c \
             $(FREERTOS)/timers.c \
             $(FREERTOS)/list.c \
             $(FREERTOS)/portable/GCC/ARM_CM4F/port.c \
             $(FREERTOS)/portable/MemMang/heap_4.c

USB_SRCS := $(USB_LIB)/Core/Src/usbd_core.c \
            $(USB_LIB)/Core/Src/usbd_ctlreq.c \
            $(USB_LIB)/Core/Src/usbd_ioreq.c \
            $(USB_LIB)/Class/CDC/Src/usbd_cdc.c

CMSIS_SRCS := $(CMSIS)/Source/Templates/system_stm32f4xx.c

C_SRCS   := $(PROJ_SRCS) $(HAL_SRCS) $(RTOS_SRCS) $(USB_SRCS) $(CMSIS_SRCS)
ASM_SRCS := $(CMSIS)/Source/Templates/gcc/startup_stm32f411xe.s

C_OBJS   := $(addprefix $(BUILD)/, $(notdir $(C_SRCS:.c=.o)))
ASM_OBJS := $(addprefix $(BUILD)/, $(notdir $(ASM_SRCS:.s=.o)))
OBJS     := $(C_OBJS) $(ASM_OBJS)

VPATH := $(ROOT)/Src \
         $(HAL_DRV)/Src \
         $(FREERTOS) \
         $(FREERTOS)/portable/GCC/ARM_CM4F \
         $(FREERTOS)/portable/MemMang \
         $(USB_LIB)/Core/Src \
         $(USB_LIB)/Class/CDC/Src \
         $(CMSIS)/Source/Templates \
         $(CMSIS)/Source/Templates/gcc

.PHONY: all flash clean size

all: $(BUILD)/firmware.elf

$(BUILD)/firmware.elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	$(OBJCOPY) -O binary $@ $(BUILD)/firmware.bin
	arm-none-eabi-size $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) -c $< -o $@ $(CFLAGS)

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) -c $< -o $@ $(CFLAGS)

$(BUILD):
	mkdir -p $(BUILD)

size: $(BUILD)/firmware.elf
	arm-none-eabi-size $<

flash: $(BUILD)/firmware.elf
	st-flash --reset write $(BUILD)/firmware.bin 0x08000000

clean:
	rm -f $(OBJS) $(BUILD)/firmware.elf $(BUILD)/firmware.bin
