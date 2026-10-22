# blackpill-GY-273

USB CDC serial console for a GY-273 magnetometer module (HMC5883L or its
QMC5883L clone) on a Black Pill board (STM32F411CEU6, 512 KiB flash,
128 KiB SRAM, 25 MHz HSE). From a terminal you can scan the I2C bus, read the
sensor once, or have it print readings at a fixed interval.

The sensor is driven by the
[i2c_magnetometer](https://github.com/crimaniak/i2c_magnetometer) library,
included as a submodule. This project adds the USB console around it.

The board enumerates as `0483:5740` "GY-273 Magnetometer Console" (on Linux
usually `/dev/ttyACM0`). Any terminal works, for example
`picocom /dev/ttyACM0` or `screen /dev/ttyACM0`. The baud rate setting has no
effect.

## Wiring

| GY-273 | Black Pill | Notes |
|---|---|---|
| VCC | 3V3 | The module has its own regulator and accepts 3.3-5 V |
| GND | G | |
| SCL | PB6 | I2C1_SCL (AF4) |
| SDA | PB7 | I2C1_SDA (AF4) |
| DRDY | PB5 | Optional, see [Data ready](#data-ready-drdy) |

The bus runs at 100 kHz. The GY-273 has 4.7 kΩ pull-ups on SDA and SCL, so no
external resistors are needed. The internal pull-ups are also enabled so an
empty bus idles high.

## Commands

| Command | Effect |
|---|---|
| `scan` | Probe addresses 0x08-0x77 and print a map of the devices that answer |
| `read` | Read the sensor once |
| `start [N]` | Print a reading every N seconds (1-3600, default 1). `start 2s` also works. The first one is printed immediately |
| `start Nms` | Print a reading every N milliseconds (5-3600000) |
| `stop` | Stop printing readings |
| `rate [HZ]` | Show or set the sensor's output rate. The closest rate the chip supports is used: 10/50/100/200 Hz on a QMC5883L (default 50), 0.75-75 Hz on an HMC5883L (default 15) |
| `help` | Show the command list |

`start` while already running changes the interval. Readings are printed on
their own line and the prompt is redrawn after each one, so you can type
`stop` while they are coming in.

## Reading rate

How often readings arrive depends on both settings:

- **`start`:** how often the firmware takes a reading.
- **`rate`:** how often the sensor makes a new sample.

With DRDY connected, each reading waits for a new sample. So if the interval
is shorter than the sensor period, readings arrive at the sensor rate. For
example, `start 10ms` at the default 50 Hz gives 50 readings/s, and `start`
then prints a note saying so. To go faster, raise the sensor rate first:

```
> rate 200
Sensor rate: 200 Hz
> start 5ms
```

Measured on the test setup:

| Sensor rate | Interval | Readings/s |
|---|---|---|
| 50 Hz | `start 20ms` | 50.3 |
| 50 Hz | `start 10ms` | 50.3 |
| 100 Hz | `start 10ms` | 100.3 |
| 200 Hz | `start 5ms` | 201 |
| 10 Hz | `start 5ms` | 10.0 |

Each reading takes about 1-2 ms on the I2C bus, and a line of output is about
75 bytes, so neither limits the rate. At 100-200 Hz the output is too fast to
read by eye. Log it to a file instead, for example
`picocom /dev/ttyACM0 --logfile mag.log`.

The rate setting is kept in RAM and goes back to the chip default after a
reset.

Example session:

```
> scan
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:                         -- -- -- -- -- 0d -- --
10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
...
Found 1 device(s):
  0x0D  QMC5883L magnetometer?
> start 3
Sensor: QMC5883L at 0x0D, DRDY on PB5 ok
Printing every 3 s. Type 'stop' to stop.
X:    376 Y:   -225 Z:   -145 mG  |B|:   462 mG  heading: 329.3 deg
X:    374 Y:   -221 Z:   -154 mG  |B|:   462 mG  heading: 329.6 deg
> stop
Stopped.
```

## Readings

| Field | Meaning |
|---|---|
| `X`, `Y`, `Z` | Field along each axis, milligauss (1 mG = 0.1 µT) |
| `\|B\|` | Total field strength. The Earth's field is roughly 250-650 mG, so this is a quick sanity check |
| `heading` | `atan2(Y, X)` in degrees, 0-360 |
| `OVERFLOW` | Shown when an axis is saturated, for example with a magnet nearby |

The heading is only meaningful with the module held level. It is not
tilt-compensated, not hard/soft-iron calibrated, and not corrected for
magnetic declination.

## Sensor support

Many GY-273 modules carry a QMC5883L clone instead of the Honeywell HMC5883L.
The two chips use different addresses and register maps. The library detects
which one is present by its ID registers when a reading is first requested:

| Chip | Marking | Address | Settings used |
|---|---|---|---|
| HMC5883L | `L883` | 0x1E | Continuous, 15 Hz, 8-sample average, ±1.3 G |
| QMC5883L | `DA 5883` | 0x0D | Continuous, 50 Hz, OSR 512, ±2 G |

`scan` marks devices at these addresses with `?`, because the address alone
does not prove which chip it is.

The QMC5883L clone on the test module has quirks: failed first reads after
each sample, writes ignored after a soft reset, and bus hangs when its status
register is polled. The library works around them; see its
[README](https://github.com/crimaniak/i2c_magnetometer#qmc5883l-clone-quirks).

## Data ready (DRDY)

With DRDY connected to PB5, every reading waits for a fresh sample. PB5 is
set up as an EXTI line whose pending flag is polled. No interrupt handler is
involved, so even the 250 µs pulse of an HMC5883L can't be missed.

When the sensor is detected, the library checks whether DRDY fires during the
first conversion. The result is shown as
`Sensor: QMC5883L at 0x0D, DRDY on PB5 ok`, or
`no signal on DRDY (PB5)`. `rate` repeats the warning if DRDY stops working.
Without DRDY:

- **HMC5883L:** the status register is polled.
- **QMC5883L:** reads are paced to the sample rate. On the test module, 1-5
  reads per 3 s failed at high rates, each followed by an automatic
  reconfiguration, which gave about 45 readings/s at 50 Hz and 140 at 200 Hz.
  Connect DRDY if you need the full rate.

## Error recovery

If a read fails, for example because a wire came loose, the firmware prints
the error, and the next reading configures the sensor again. While streaming
it keeps retrying at the set interval, so reconnecting the module resumes the
output. After an I2C timeout the bus is recovered right away: SCL is clocked
by hand to free a slave holding SDA low, then the I2C peripheral is reset
(`i2c_bus_init()`, passed to the library as its recovery hook).

## Layout

- `Src/main.c`: clocks, USB re-enumeration (PA12 low for 100 ms), I2C init, RTOS objects, console task
- `Src/console.c`: command line, output formatting, streaming schedule, library setup (STM32 HAL port on I2C1, DRDY on PB5)
- `Src/i2c_bus.c`: I2C1 setup on PB6/PB7, bus recovery, bus scan
- `Lib/i2c_magnetometer`: the sensor library (submodule)
- `Src/usbd_conf.c`, `Src/usbd_desc.c`, `Src/usbd_cdc_if.c`: USB device glue
- `Src/stm32f4xx_it.c`: `SysTick_Handler` and `OTG_FS_IRQHandler`
- `STM32F411CE_FLASH.ld`: linker script

Clocks: 25 MHz HSE, PLL M=25 N=192 P=2 Q=4, giving 96 MHz SYSCLK, 48 MHz for
USB, and 48 MHz APB1 for I2C1. USB uses OTG_FS in device mode with VBUS sensing
off. The firmware runs on FreeRTOS (`ARM_CM4F` port, 20 KiB heap) with a single
console task. USB is interrupt-driven.

## Dependencies

All libraries are git submodules (`git clone --recurse-submodules`, or
`git submodule update --init` after a plain clone):

| Path | Upstream | Version |
|---|---|---|
| `Middlewares/FreeRTOS` | FreeRTOS/FreeRTOS-Kernel | V11.3.2 |
| `Middlewares/stm32_mw_usb_device` | STMicroelectronics/stm32_mw_usb_device | v2.11.6 |
| `Drivers/stm32f4xx-hal-driver` | STMicroelectronics/stm32f4xx-hal-driver | v1.8.5-12 (`1f6451c`) |
| `Drivers/cmsis-device-f4` | STMicroelectronics/cmsis-device-f4 | v2.6.11-3 (`9192c7b`) |
| `Drivers/cmsis-core` | STMicroelectronics/cmsis-core | v5.9.0_20250520 |
| `Lib/i2c_magnetometer` | crimaniak/i2c_magnetometer | main |

You also need `arm-none-eabi-gcc` and `st-flash` (stlink tools).

## Build and flash

```sh
make            # build/firmware.elf and build/firmware.bin
make flash      # st-flash write to 0x08000000, then reset
make clean
```

The firmware builds to about 35 KB of flash and 26 KB of RAM.

## Status

Tested on a Black Pill (STM32F411CEU6) with a GY-273 module carrying a
QMC5883L clone:

- All commands work, including typing a command while readings are printing,
  at every interval down to 5 ms.
- With DRDY, readings arrive at the expected rate at all four sensor rates
  with no errors (see [Reading rate](#reading-rate)). The DRDY check passed
  in 200 of 200 rate changes.
- 40 of 40 back-to-back rate changes (each a full sensor reset and setup)
  were followed by a good reading, with and without DRDY.
- Without DRDY, see [Data ready](#data-ready-drdy).

In one early test run at 200 Hz there was a burst of read errors that looked
like DRDY not being detected. It did not happen again in later runs, including
the 200 rate changes above.

The HMC5883L code path has been tested only against a simulated chip,
because no module with a genuine HMC5883L was available.
