# blackpill-GY-273

USB CDC serial console for a GY-273 magnetometer module (HMC5883L or its
QMC5883L clone) on a Black Pill board (STM32F411CEU6, 512 KiB flash,
128 KiB SRAM, 25 MHz HSE). From a terminal you can scan the I2C bus, read the
sensor once, or have it print readings at a fixed interval.

An optional 0.96" OLED shows the heading and field strength, also without a
terminal connected.

The sensor is driven by the
[i2c_magnetometer](https://github.com/crimaniak/i2c_magnetometer) library and
the display by [stm32-ssd1306](https://github.com/afiskon/stm32-ssd1306),
both included as submodules. This project adds the USB console and the
display screens around them.

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

### Display (optional)

A 0.96" 128x64 OLED with an SSD1306 or SSD1315 controller, 7-pin SPI version
(for example JMD096-1315-W):

| OLED | Black Pill | Notes |
|---|---|---|
| GND | G | |
| VCC | 3V3 | The module has its own regulator |
| D0 | PA5 | SPI1_SCK (AF5) |
| D1 | PA7 | SPI1_MOSI (AF5) |
| RES | PB1 | Reset, pulsed low at startup |
| DC | PB0 | Command/data select |
| CS | PA4 | Chip select |

SPI1 runs at 6 MHz in mode 0. The display is optional and detected at
startup (see [Display](#display)).

## Commands

| Command | Effect |
|---|---|
| `scan` | Probe addresses 0x08-0x77 and print a map of the devices that answer |
| `read` | Read the sensor once |
| `start [N]` | Print a reading every N seconds (1-3600, default 1). `start 2s` also works. The first one is printed immediately |
| `start Nms` | Print a reading every N milliseconds (5-3600000) |
| `stop` | Stop printing readings |
| `rate [HZ]` | Show or set the sensor's output rate. The closest rate the chip supports is used: 10/50/100/200 Hz on a QMC5883L (default 50), 0.75-75 Hz on an HMC5883L (default 15) |
| `cal` | Show the calibration in effect |
| `cal start [N]` | Calibrate: turn the module through all orientations for N seconds (5-300, default 30) |
| `cal stop` | Finish calibrating early |
| `cal clear` | Remove the calibration; readings are raw again |
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

Once a calibration is made with `cal start`, it is applied to all readings,
including `|B|` and `heading`. The heading is only meaningful with the module
held level. It is not tilt-compensated (there is no accelerometer) and not
corrected for magnetic declination.

## Display

The display can't be read back over SPI, so the firmware detects it by its
pull-up resistors instead. These modules have pull-ups on D0 and D1 (for
their I2C mode). At startup the firmware reads both pins with the STM32's
internal pull-downs:
- **Both high:** a display is attached. The firmware sets up SPI and refreshes
  it.
- **Otherwise:** there is no display. The firmware sends no SPI traffic and
  checks again every 2 seconds, so a display plugged in later is picked up
  without a reset.

The terminal banner shows the result (`Display: connected` or
`Display: not found`). Unplugging a display while it is in use is not
detected, because its pins are then driven by SPI; that takes a reset.

The display refreshes 5 times a second. It works without a terminal open,
so the board can run as a standalone compass on USB power.

```
 223.0°  SW          heading (level, uncalibrated or calibrated) and compass point
 |B| 550 mG     RAW  field strength; CAL once a calibration is applied
 X -388 Y -364 Z  130     field per axis, mG
 QMC5883L 50Hz DRDY       sensor, rate, DRDY status (or OVERFLOW)
```

During `cal start` it shows a bar per axis with how much of a full turn the
axis has seen, and the seconds left. Without a sensor it shows `No sensor`.
The heading on the display is the direction of the GY-273's X axis, so mount
the module with its X arrow pointing where the display should point.

When the terminal is printing readings, the display uses the latest of them;
otherwise it reads the sensor itself.

## Calibration

Everything fixed near the sensor (the board, wires, the USB cable, screws, a
case) adds its own field or bends the Earth's. That shifts and stretches the
readings and skews the heading. Calibration measures this and corrects it.

1. **Mount the module where it will stay.** Calibration is only valid for
   the surroundings it was made in. Keep away from large steel objects that
   won't travel with the module, such as a steel desk frame or a PC case.
2. **Run `cal start`** (30 s, or `cal start N`) and slowly turn the module
   through all orientations: a full turn flat, a full turn on its side, then
   on another side, and slow figure-eights.
3. **Watch the progress line,** printed once a second. For each axis it
   shows how much of a full turn the axis has seen. Keep turning until all
   three are near 100%:
   ```
   Turning: X 100% Y  87% Z  41%   12 s left
   ```
   A low Z usually means the module was only turned flat.
4. **When time is up,** or after `cal stop`, the result is printed and
   applied:
   ```
   Calibration done (1480 samples, X 100% Y  97% Z  95%), now applied to readings:
     offset: X  -106 Y    51 Z   200 mG
     scale:  X 0.894 Y 1.067 Z 1.059
   ```
   If an axis stayed below 60%, the calibration is rejected and the previous
   one, if any, stays in effect.
5. **Check:** run `start 200ms` and turn the module. `|B|` should now stay
   nearly constant in every direction.

Starting a calibration stops `start` printing, and `start` is refused while
calibrating. The calibration is kept in RAM only, so it is lost on reset or
power-off; run it again after that.

The method is min/max: offsets (hard iron) and per-axis scale (simplified
soft iron), from the library's `mag_cal_collect_*` functions. It does not
correct distortion skewed between the axes, which needs a full ellipsoid
fit.

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
- `Src/console.c`: command line, output formatting, streaming and calibration schedule, library setup (STM32 HAL port on I2C1, DRDY on PB5)
- `Src/i2c_bus.c`: I2C1 setup on PB6/PB7, bus recovery, bus scan
- `Src/display.c`: SPI1 setup and the display screens
- `Src/fmt.c`: number formatting for console and display (the firmware has no printf)
- `Inc/ssd1306_conf.h`: display driver configuration (pins, fonts)
- `Lib/i2c_magnetometer`: the sensor library (submodule)
- `Lib/stm32-ssd1306`: the display driver (submodule)
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
| `Lib/stm32-ssd1306` | afiskon/stm32-ssd1306 | master (`25bd2e9`) |

You also need `arm-none-eabi-gcc` and `st-flash` (stlink tools).

## Build and flash

```sh
make            # build/firmware.elf and build/firmware.bin
make flash      # st-flash write to 0x08000000, then reset
make clean
```

The firmware builds to about 55 KB of flash (12 KB of it display fonts) and
27 KB of RAM.

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
- Calibration commands: rejecting too little rotation (module lying still),
  `cal stop`, `cal clear`, and their interplay with `start` were tested on
  the board. The success path (report, applying the result to readings) was
  tested on the board with simulated rotation data substituted for the
  sensor during collection. A real hand-rotated calibration is still to be
  done.
- Display (JMD096-1315-W, SSD1315): shows the screens as intended. Detection
  was tested connected at startup, not connected at startup (no SPI traffic,
  everything else works), and plugged in later without a reset.
- Loose GY-273 wire: readings resume by themselves when the connection is
  restored (this used to need a `scan`, fixed in the library).

During testing there were occasional single read errors and a few bursts of
them at 200 Hz. The test module's wiring later turned out to be loose, which
is the likely cause, but this is not proven. With the wiring fixed, 50 Hz and
200 Hz runs were clean.

The HMC5883L code path has been tested only against a simulated chip,
because no module with a genuine HMC5883L was available.
