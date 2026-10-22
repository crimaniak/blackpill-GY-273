# blackpill-GY-273

USB CDC serial console for a GY-273 magnetometer module (HMC5883L or its
QMC5883L clone) on a Black Pill board (STM32F411CEU6, 512 KiB flash,
128 KiB SRAM, 25 MHz HSE). From a terminal you can scan the I2C bus, read the
sensor once, or have it print readings at a fixed interval.

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
| `rate [HZ]` | Show or set the sensor's output rate: 10, 50, 100 or 200 Hz (default 50) |
| `help` | Show the command list |

`start` while already running changes the interval. Readings are printed on
their own line and the prompt is redrawn after each one, so you can type
`stop` while they are coming in.

## Reading rate

How often readings arrive depends on both settings:

- **`start`:** how often the firmware takes a reading.
- **`rate`:** how often the QMC5883L makes a new sample.

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
| 200 Hz | `start 5ms` | 198.7 |
| 10 Hz | `start 5ms` | 10.0 |

Each reading takes about 1-2 ms on the I2C bus, and a line of output is about
75 bytes, so neither limits the rate. At 100-200 Hz the output is too fast to
read by eye. Log it to a file instead, for example
`picocom /dev/ttyACM0 --logfile mag.log`.

The rate setting is kept in RAM and goes back to 50 Hz after a reset. The
HMC5883L is read one measurement at a time, so `rate` does not apply to it.

Example session:

```
> scan
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:                         -- -- -- -- -- 0d -- --
10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
...
Found 1 device(s):
  0x0D  QMC5883L magnetometer
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
The two chips use different addresses and register maps. The firmware detects
which one is present when a reading is first requested:

| Chip | Marking | Address | Identified by | Settings |
|---|---|---|---|---|
| HMC5883L | `L883` | 0x1E | ID registers 0x0A-0x0C = `H43` | Single measurement per reading, 8-sample average, ±1.3 Ga (1090 LSB/Ga) |
| QMC5883L | `DA 5883` | 0x0D | Register 0x0D = 0xFF | Continuous at 10/50/100/200 Hz (default 50), OSR 512, ±2 G (12000 LSB/G) |

### QMC5883L clone quirks

The QMC5883L on the module this was tested with behaves differently from its
datasheet. The firmware works around both quirks:

- **The first burst read after each new sample fails.** The chip returns only
  the first byte and then 0xFF, whether the read uses a repeated start or a
  STOP after the register address. The next burst returns the whole sample.
  The firmware reads the 6 data bytes together with the status byte. A real
  status byte always has its upper 5 bits clear, so `0xFF` marks a failed
  burst, and the firmware retries it (up to 4 tries).
- **The chip ignores writes for a while after a soft reset.** If it was
  already measuring, it NACKs writes for more than 5 ms after the soft reset.
  Setup retries the configuration for up to 50 ms and reads CTRL1 back to
  confirm it.

## Data ready (DRDY)

With DRDY connected to PB5, every reading uses a fresh sample:

- **HMC5883L:** each reading starts a single measurement and waits for the
  250 µs low pulse on DRDY.
- **QMC5883L:** the chip measures continuously at 50 Hz. Each reading waits
  until DRDY is high, which shows that a new sample has not been read yet.

PB5 is set up as an EXTI line, and the firmware checks its pending flag
directly. No interrupt handler is involved, so even the HMC's short pulse
can't be missed.

When the sensor is detected, the firmware checks whether DRDY changes during
the first conversion and reports the result:
`Sensor: QMC5883L at 0x0D, DRDY on PB5 ok`. If DRDY doesn't change, the
message ends with `no signal on DRDY (PB5)` and the firmware falls back:

- **HMC5883L:** polls the status register (RDY bit).
- **QMC5883L:** reads the data registers without waiting. They always hold
  the latest sample in continuous mode. Polling the status register is
  avoided because it made the tested clone hang the bus now and then.

Without DRDY, a QMC5883L read can land in the middle of a sample update. That
failed about 1 time in 400 in testing. A failed read prints
`Error: sensor read failed`, and the next reading detects the sensor again.

## Error recovery

If a read fails, for example because a wire came loose, the firmware resets the
I2C bus and detects the sensor again on the next attempt. An I2C timeout
leaves the STM32 I2C peripheral stuck with BUSY set, so after a timeout the
bus is reset right away. While streaming it
keeps retrying at the set interval, so reconnecting the module resumes the
output. Bus recovery clocks SCL by hand to free a slave holding SDA low, then
resets the I2C peripheral.

## Layout

- `Src/main.c`: clocks, USB re-enumeration (PA12 low for 100 ms), I2C init, RTOS objects, console task
- `Src/console.c`: command line, output formatting, streaming schedule
- `Src/i2c_bus.c`: I2C1 setup on PB6/PB7, bus recovery, bus scan
- `Src/magnetometer.c`: HMC5883L / QMC5883L detection, configuration, DRDY handling and readout
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

You also need `arm-none-eabi-gcc` and `st-flash` (stlink tools).

## Build and flash

```sh
make            # build/firmware.elf and build/firmware.bin
make flash      # st-flash write to 0x08000000, then reset
make clean
```

The firmware builds to about 33 KB of flash and 26 KB of RAM.

## Status

Tested on a Black Pill (STM32F411CEU6) with a GY-273 module carrying a
QMC5883L clone, with DRDY connected:

- All commands work, including typing a command while readings are printing,
  at every interval down to 5 ms.
- Readings arrive at the expected rate at all four sensor rates (see
  [Reading rate](#reading-rate)).
- Readings are stable, and the total field of about 460-490 mG matches the
  Earth's field.
- 300 of 300 back-to-back reads succeeded.
- Sensor setup succeeded 40 of 40 times, and also on the first read after a
  reflash.
- The no-DRDY fallback was tested by forcing it in firmware: 399 of 400 reads
  succeeded.

The HMC5883L code path has not been tested on hardware because no module with
a genuine HMC5883L was available.
