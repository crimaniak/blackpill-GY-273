#include "console.h"
#include "i2c_bus.h"
#include "i2c_magnetometer.h"
#include "mag_compass.h"
#include "mag_driver.h"
#include "mag_port_stm32_hal.h"
#include "usbd_cdc_if.h"
#include "usbd_cdc.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdlib.h>

#define BANNER          "\r\nGY-273 Magnetometer Console\r\n"
#define MIN_INTERVAL_MS 5          /* 200 Hz, the fastest sensor rate */
#define MAX_INTERVAL_MS 3600000    /* 1 hour */

extern USBD_HandleTypeDef hUsbDeviceFS;

static char       s_line[64];       /* command line being typed */
static int        s_pos;
static TickType_t s_interval;       /* streaming period in ticks, 0 = stopped */
static TickType_t s_next_sample;

/* Magnetometer on I2C1, DRDY on PB5 */
static mag_stm32_port_t s_port = {
    .hi2c      = &g_i2c,
    .drdy_port = GPIOB,
    .drdy_pin  = GPIO_PIN_5,
    .recover   = i2c_bus_init,   /* also frees a slave holding SDA low */
};
static mag_bus_t s_bus;
static mag_dev_t s_mag;

/* Transmit over USB CDC. Data is copied into s_tx_buf after the previous
   transfer has finished, so callers may pass stack buffers. */
static void cdc_write_raw(const uint8_t *buf, uint32_t len) {
    static uint8_t s_tx_buf[256];
    while (len) {
        uint32_t n = len < sizeof(s_tx_buf) ? len : sizeof(s_tx_buf);
        /* Wait for previous transfer to complete (100 ms max). */
        if (xSemaphoreTake(g_cdc_tx_done, pdMS_TO_TICKS(100)) != pdTRUE)
            return;
        memcpy(s_tx_buf, buf, n);
        USBD_CDC_SetTxBuffer(&hUsbDeviceFS, s_tx_buf, n);
        if (USBD_CDC_TransmitPacket(&hUsbDeviceFS) != USBD_OK)
            xSemaphoreGive(g_cdc_tx_done);   /* restore if transmit failed */
        buf += n;
        len -= n;
    }
}

static void cdc_write(const char *s) {
    cdc_write_raw((const uint8_t *)s, (uint32_t)strlen(s));
}

/* Append v right-aligned in a field of width chars; returns the new end. */
static char *fmt_int(char *p, int32_t v, int width) {
    char tmp[12];
    int  n = 0;
    uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) tmp[n++] = '-';
    for (int i = n; i < width; i++) *p++ = ' ';
    while (n) *p++ = tmp[--n];
    return p;
}

static char *fmt_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

static char *fmt_hex8(char *p, uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    *p++ = '0';
    *p++ = 'x';
    *p++ = hex[v >> 4];
    *p++ = hex[v & 0xF];
    return p;
}

/* Append v with up to 2 decimals: "50", "7.5", "0.75" */
static char *fmt_dec2(char *p, float v) {
    int32_t c = (int32_t)(v * 100.0f + 0.5f);
    p = fmt_int(p, c / 100, 0);
    if (c % 100) {
        *p++ = '.';
        *p++ = (char)('0' + c % 100 / 10);
        if (c % 10)
            *p++ = (char)('0' + c % 10);
    }
    return p;
}

static int32_t round_i32(float v) {
    return (int32_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

/* Name of a chip the library knows at this address, or NULL */
static const char *known_chip(uint8_t addr) {
    for (const mag_driver_t *const *d = mag_builtin_drivers; *d; d++)
        for (int i = 0; i < (int)sizeof((*d)->addrs) && (*d)->addrs[i]; i++)
            if ((*d)->addrs[i] == addr)
                return (*d)->name;
    return NULL;
}

/* Detect the sensor if needed. Returns 1 when one is ready. */
static int ensure_sensor(void) {
    if (!s_bus.read) {
        mag_port_stm32_init(&s_port, &s_bus);
        mag_init(&s_mag, &s_bus);
    }
    if (s_mag.drv)
        return 1;
    if (mag_detect(&s_mag, NULL) != MAG_OK) {
        cdc_write("Error: no supported magnetometer found. "
                  "Check wiring, try 'scan'.\r\n");
        return 0;
    }
    char buf[96];
    char *p = fmt_str(buf, "Sensor: ");
    p = fmt_str(p, mag_chip_name(&s_mag));
    p = fmt_str(p, " at ");
    p = fmt_hex8(p, s_mag.addr);
    p = fmt_str(p, s_mag.drdy_ok ? ", DRDY on PB5 ok\r\n" : ", no signal on DRDY (PB5)\r\n");
    *p = '\0';
    cdc_write(buf);
    return 1;
}

/* Read the sensor and append one line to p:
   X:   -123 Y:     45 Z:   -380 mG  |B|:   402 mG  heading: 159.8 deg
   or an error message. The sensor must already be detected. */
static char *fmt_sample(char *p) {
    mag_sample_t s;
    int err = mag_read(&s_mag, &s);
    if (err != MAG_OK) {
        p = fmt_str(p, "Error: ");
        p = fmt_str(p, mag_strerror(err));
        return fmt_str(p, "\r\n");
    }

    /* 1 uT = 10 mG */
    int32_t heading_d10 = round_i32(mag_heading_deg(&s, 0.0f) * 10.0f) % 3600;
    p = fmt_str(p, "X:");
    p = fmt_int(p, round_i32(s.x * 10.0f), 7);
    p = fmt_str(p, " Y:");
    p = fmt_int(p, round_i32(s.y * 10.0f), 7);
    p = fmt_str(p, " Z:");
    p = fmt_int(p, round_i32(s.z * 10.0f), 7);
    p = fmt_str(p, " mG  |B|:");
    p = fmt_int(p, round_i32(mag_field_ut(&s) * 10.0f), 6);
    p = fmt_str(p, " mG  heading:");
    p = fmt_int(p, heading_d10 / 10, 4);
    *p++ = '.';
    *p++ = (char)('0' + heading_d10 % 10);
    p = fmt_str(p, " deg");
    if (s.flags & MAG_FLAG_OVERFLOW)
        p = fmt_str(p, "  OVERFLOW");
    return fmt_str(p, "\r\n");
}

static void print_sample(void) {
    if (!ensure_sensor())
        return;
    char buf[96];
    *fmt_sample(buf) = '\0';
    cdc_write(buf);
}

/* Print a streamed reading without mangling the line being typed: erase the
   prompt, print the reading, then redraw the prompt and the partial input.
   All of it goes out in one write to keep up at high rates. */
static void stream_sample(void) {
    if (!s_mag.drv) {
        cdc_write("\r\x1b[K");
        ensure_sensor();
        cdc_write("> ");
        cdc_write_raw((const uint8_t *)s_line, (uint32_t)s_pos);
        return;
    }
    char buf[96 + sizeof(s_line) + 8];
    char *p = fmt_str(buf, "\r\x1b[K");
    p = fmt_sample(p);
    p = fmt_str(p, "> ");
    memcpy(p, s_line, (size_t)s_pos);
    p += s_pos;
    cdc_write_raw((const uint8_t *)buf, (uint32_t)(p - buf));
}

/* Append an interval as "3 s" or "250 ms". */
static char *fmt_interval(char *p, uint32_t ms) {
    if (ms % 1000 == 0)
        return fmt_str(fmt_int(p, (int32_t)(ms / 1000), 0), " s");
    return fmt_str(fmt_int(p, (int32_t)ms, 0), " ms");
}

static void cmd_scan(void) {
    uint8_t found[16];
    int n = i2c_bus_scan(cdc_write, found, (int)sizeof(found));
    if (n < 0) {
        cdc_write("Error: bus stuck busy (SDA or SCL held low?). Bus reset.\r\n");
        i2c_bus_init();
        return;
    }
    if (n == 0) {
        cdc_write("No devices found.\r\n");
        return;
    }

    char buf[48];
    char *p = fmt_str(buf, "Found ");
    p = fmt_int(p, n, 0);
    p = fmt_str(p, " device(s):\r\n");
    *p = '\0';
    cdc_write(buf);
    for (int i = 0; i < n && i < (int)sizeof(found); i++) {
        const char *name = known_chip(found[i]);
        p = fmt_str(buf, "  ");
        p = fmt_hex8(p, found[i]);
        if (name) {
            p = fmt_str(p, "  ");
            p = fmt_str(p, name);
            p = fmt_str(p, " magnetometer?");
        }
        p = fmt_str(p, "\r\n");
        *p = '\0';
        cdc_write(buf);
    }
}

/* start [N | Ns | Nms]: N alone is seconds */
static void cmd_start(const char *arg) {
    long ms = 1000;
    while (*arg == ' ') arg++;
    if (*arg) {
        char *end;
        long n = strtol(arg, &end, 10);
        int  ok = end != arg;
        while (*end == ' ') end++;
        if (strcmp(end, "ms") == 0)
            ms = n;
        else if (*end == '\0' || strcmp(end, "s") == 0)
            ms = n * 1000;
        else
            ok = 0;
        if (!ok || n < 1 || ms < MIN_INTERVAL_MS || ms > MAX_INTERVAL_MS) {
            cdc_write("Usage: start [N]     every N seconds (1-3600)\r\n"
                      "       start Nms     every N milliseconds (5-3600000)\r\n");
            return;
        }
    }
    if (!ensure_sensor())
        return;

    s_interval    = pdMS_TO_TICKS((uint32_t)ms);
    s_next_sample = xTaskGetTickCount();   /* first reading right away */

    char buf[160];
    char *p = fmt_str(buf, "Printing every ");
    p = fmt_interval(p, (uint32_t)ms);
    p = fmt_str(p, ". Type 'stop' to stop.\r\n");
    uint32_t period_ms = (uint32_t)(1000.0f / s_mag.cfg.rate_hz);
    if (s_mag.cfg.mode == MAG_MODE_CONTINUOUS && (uint32_t)ms < period_ms) {
        p = fmt_str(p, "Note: at ");
        p = fmt_dec2(p, s_mag.cfg.rate_hz);
        p = fmt_str(p, " Hz the sensor has a new sample every ");
        p = fmt_int(p, (int32_t)period_ms, 0);
        p = fmt_str(p, s_mag.drdy_ok || s_mag.drv->data_ready ? " ms, readings wait for it.\r\n"
                                                              : " ms, readings repeat.\r\n");
        p = fmt_str(p, "Use 'rate' to make it faster.\r\n");
    }
    *p = '\0';
    cdc_write(buf);
}

/* rate [HZ]: the sensor uses the closest rate it supports */
static void cmd_rate(const char *arg) {
    while (*arg == ' ') arg++;
    if (!ensure_sensor())
        return;
    if (*arg) {
        char *end;
        long hz = strtol(arg, &end, 10);
        while (*end == ' ') end++;
        if (end == arg || *end != '\0' || hz < 1 || hz > 1000) {
            cdc_write("Usage: rate [HZ]   HZ = 1-1000, the closest supported rate is used\r\n");
            return;
        }
        mag_config_t cfg = s_mag.req;
        cfg.rate_hz = (float)hz;
        int err = mag_configure(&s_mag, &cfg);
        if (err != MAG_OK) {
            cdc_write("Error: ");
            cdc_write(mag_strerror(err));
            cdc_write("\r\n");
            return;
        }
    }

    char buf[96];
    char *p = fmt_str(buf, "Sensor rate: ");
    p = fmt_dec2(p, s_mag.cfg.rate_hz);
    p = fmt_str(p, s_mag.drdy_ok ? " Hz\r\n" : " Hz, no signal on DRDY (PB5)\r\n");
    *p = '\0';
    cdc_write(buf);
}

static void process_command(const char *line) {
    while (*line == ' ') line++;
    if (line[0] == '\0') return;

    if (strcmp(line, "help") == 0) {
        cdc_write("Commands:\r\n"
                  "  scan        scan the I2C bus (PB6 = SCL, PB7 = SDA)\r\n"
                  "  read        read the sensor once\r\n"
                  "  start [N]   print readings every N seconds (1-3600, default 1)\r\n"
                  "  start Nms   print readings every N milliseconds (5-3600000)\r\n"
                  "  stop        stop printing readings\r\n"
                  "  rate [HZ]   show or set the sensor rate (closest supported)\r\n"
                  "  help        this help\r\n");
        return;
    }

    if (strcmp(line, "scan") == 0) {
        cmd_scan();
        return;
    }

    if (strcmp(line, "read") == 0) {
        print_sample();
        return;
    }

    if (strcmp(line, "start") == 0 || strncmp(line, "start ", 6) == 0) {
        cmd_start(line + 5);
        return;
    }

    if (strcmp(line, "rate") == 0 || strncmp(line, "rate ", 5) == 0) {
        cmd_rate(line + 4);
        return;
    }

    if (strcmp(line, "stop") == 0) {
        cdc_write(s_interval ? "Stopped.\r\n" : "Not running.\r\n");
        s_interval = 0;
        return;
    }

    cdc_write("Unknown command. Type 'help'.\r\n");
}

void ConsoleTask(void *pvParam) {
    (void)pvParam;

    /* Wait for the host to open the terminal (DTR assert) */
    while (!g_cdc_connected)
        vTaskDelay(pdMS_TO_TICKS(100));

    cdc_write(BANNER "Type 'help' for commands.\r\n> ");

    int last_cr = 0;   /* previous byte was '\r': swallow a following '\n' (CRLF) */

    for (;;) {
        /* Block up to 200 ms between bytes (gives reconnect logic a chance
           to run), or less if a streamed reading is due sooner. */
        TickType_t wait = pdMS_TO_TICKS(200);
        if (s_interval && g_cdc_connected) {
            TickType_t now  = xTaskGetTickCount();
            int32_t    left = (int32_t)(s_next_sample - now);
            if (left <= 0) {
                stream_sample();
                /* The reading itself can take up to a sensor period (it
                   waits for DRDY), so compare against the time after it.
                   If behind, skip ahead instead of reading back to back,
                   so typed commands are still handled. */
                now = xTaskGetTickCount();
                s_next_sample += s_interval;
                if ((int32_t)(s_next_sample - now) <= 0)
                    s_next_sample = now + s_interval;
                continue;
            }
            if ((TickType_t)left < wait)
                wait = (TickType_t)left;
        }

        uint8_t ch;
        if (xQueueReceive(g_cdc_rx_queue, &ch, wait) != pdTRUE) {
            /* Timeout — check if host disconnected and reconnected */
            if (!g_cdc_connected) {
                while (!g_cdc_connected)
                    vTaskDelay(pdMS_TO_TICKS(100));
                s_pos = 0;
                cdc_write(BANNER "> ");
            }
            continue;
        }

        int was_cr = last_cr;
        last_cr = (ch == '\r');

        if (ch == '\n' && was_cr) {
            continue;   /* second half of CRLF */
        } else if (ch == '\r' || ch == '\n') {
            cdc_write("\r\n");
            s_line[s_pos] = '\0';
            process_command(s_line);
            s_pos = 0;
            cdc_write("> ");
        } else if ((ch == '\b' || ch == 127) && s_pos > 0) {
            s_pos--;
            cdc_write("\b \b");
        } else if (ch >= 0x20 && s_pos < (int)sizeof(s_line) - 1) {
            s_line[s_pos++] = (char)ch;
            cdc_write_raw(&ch, 1);
        }
    }
}
