#include "console.h"
#include "display.h"
#include "fmt.h"
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

#define CAL_DEFAULT_S   30
#define CAL_MAX_S       300
#define CAL_SAMPLE_MS   20         /* collect at up to 50 samples/s */
#define CAL_REPORT_MS   1000       /* progress line once a second */
#define CAL_MIN_COVER   60         /* % of a full turn each axis must see */

#define DISPLAY_MS      200        /* display refresh, 5 per second */
#define DETECT_RETRY_MS 1000       /* look for a missing sensor this often */
#define DISPLAY_RETRY_MS 2000      /* look for a missing display this often */

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
static int        s_announced;       /* "Sensor: ..." printed to the terminal */
static TickType_t s_next_detect;

/* Last good reading, shared with the display */
static mag_sample_t s_last;
static int          s_have_last;
static TickType_t   s_last_tick;
static TickType_t   s_next_display;
static int          s_display;         /* display found and initialised */

/* Calibration in progress (cal start) */
static int                 s_cal_active;
static TickType_t          s_cal_end, s_cal_next_sample, s_cal_next_report;
static mag_cal_collector_t s_cal;
static float               s_cal_field_sum;   /* sum of |B|, uT, for coverage */

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

/* Name of a chip the library knows at this address, or NULL */
static const char *known_chip(uint8_t addr) {
    for (const mag_driver_t *const *d = mag_builtin_drivers; *d; d++)
        for (int i = 0; i < (int)sizeof((*d)->addrs) && (*d)->addrs[i]; i++)
            if ((*d)->addrs[i] == addr)
                return (*d)->name;
    return NULL;
}

/* Detect the sensor if needed, without printing. With force = 0 a missing
   sensor is looked for at most once per DETECT_RETRY_MS. */
static int sensor_ready(int force) {
    if (!s_bus.read) {
        mag_port_stm32_init(&s_port, &s_bus);
        mag_init(&s_mag, &s_bus);
    }
    if (s_mag.drv)
        return 1;
    TickType_t now = xTaskGetTickCount();
    if (!force && (int32_t)(s_next_detect - now) > 0)
        return 0;
    s_next_detect = now + pdMS_TO_TICKS(DETECT_RETRY_MS);
    return mag_detect(&s_mag, NULL) == MAG_OK;
}

/* Detect the sensor if needed. Returns 1 when one is ready. Tells the
   terminal which sensor was found, once. */
static int ensure_sensor(void) {
    if (!sensor_ready(1)) {
        cdc_write("Error: no supported magnetometer found. "
                  "Check wiring, try 'scan'.\r\n");
        return 0;
    }
    if (s_announced)
        return 1;
    s_announced = 1;
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
    s_last      = s;
    s_have_last = 1;
    s_last_tick = xTaskGetTickCount();

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

/* Print text that was not asked for by a command (streamed readings,
   calibration progress) without mangling the line being typed: erase the
   prompt, print the text, then redraw the prompt and the partial input.
   All of it goes out in one write to keep up at high rates. */
static void print_async(const char *text) {
    char buf[320];
    size_t n = strlen(text);
    if (n > sizeof(buf) - sizeof(s_line) - 8)
        n = sizeof(buf) - sizeof(s_line) - 8;
    char *p = fmt_str(buf, "\r\x1b[K");
    memcpy(p, text, n);
    p = fmt_str(p + n, "> ");
    memcpy(p, s_line, (size_t)s_pos);
    p += s_pos;
    cdc_write_raw((const uint8_t *)buf, (uint32_t)(p - buf));
}

static void stream_sample(void) {
    if (!s_mag.drv) {
        cdc_write("\r\x1b[K");
        ensure_sensor();
        cdc_write("> ");
        cdc_write_raw((const uint8_t *)s_line, (uint32_t)s_pos);
        return;
    }
    char buf[128];
    *fmt_sample(buf) = '\0';
    print_async(buf);
}

/* ---- calibration ---- */

/* How much of a full turn each axis has seen, in %: its span compared with
   the span of a full turn, 2 x the field strength. */
static void cal_coverage(int cover[3]) {
    float field = s_cal.count ? s_cal_field_sum / (float)s_cal.count : 0.0f;
    for (int i = 0; i < 3; i++) {
        float span = s_cal.max[i] - s_cal.min[i];
        int c = field > 0.0f ? (int)(span / (2.0f * field) * 100.0f) : 0;
        cover[i] = c > 100 ? 100 : c;
    }
}

static char *fmt_coverage(char *p) {
    static const char axes[] = "XYZ";
    int cover[3];
    cal_coverage(cover);
    for (int i = 0; i < 3; i++) {
        *p++ = ' ';
        *p++ = axes[i];
        p = fmt_int(p, cover[i], 4);
        *p++ = '%';
    }
    return p;
}

static void cal_sample(void) {
    mag_sample_t s;
    if (mag_read_uncalibrated(&s_mag, &s) != MAG_OK || (s.flags & MAG_FLAG_OVERFLOW))
        return;
    mag_cal_collect_add(&s_cal, &s);
    s_cal_field_sum += mag_field_ut(&s);
}

static void cal_progress(void) {
    int32_t left_ms = (int32_t)(s_cal_end - xTaskGetTickCount()) * portTICK_PERIOD_MS;
    char buf[96];
    char *p = fmt_str(buf, "Turning:");
    p = fmt_coverage(p);
    p = fmt_str(p, "   ");
    p = fmt_int(p, (left_ms + 999) / 1000, 0);
    p = fmt_str(p, " s left\r\n");
    *p = '\0';
    print_async(buf);
}

/* Append the calibration in effect, or a note that there is none */
static char *fmt_calibration(char *p) {
    static const char axes[] = "XYZ";
    if (!s_mag.has_cal)
        return fmt_str(p, "No calibration: readings are raw. Use 'cal start'.\r\n");
    p = fmt_str(p, "  offset:");
    for (int i = 0; i < 3; i++) {
        *p++ = ' ';
        *p++ = axes[i];
        p = fmt_int(p, round_i32(s_mag.cal.offset[i] * 10.0f), 6);
    }
    p = fmt_str(p, " mG\r\n  scale: ");
    for (int i = 0; i < 3; i++) {
        *p++ = ' ';
        *p++ = axes[i];
        *p++ = ' ';
        p = fmt_fixed3(p, s_mag.cal.matrix[i][i]);
    }
    return fmt_str(p, "\r\n");
}

/* Stop collecting and apply the result. Writes the report into p. */
static char *cal_finish(char *p) {
    int cover[3];
    mag_calibration_t cal;

    s_cal_active = 0;
    cal_coverage(cover);
    int enough = cover[0] >= CAL_MIN_COVER && cover[1] >= CAL_MIN_COVER &&
                 cover[2] >= CAL_MIN_COVER;
    if (!enough || mag_cal_collect_finish(&s_cal, &cal) != MAG_OK) {
        p = fmt_str(p, "Calibration failed, not enough rotation:");
        p = fmt_coverage(p);
        p = fmt_str(p, " (need ");
        p = fmt_int(p, CAL_MIN_COVER, 0);
        p = fmt_str(p, "% each).\r\nTurn the module so that every axis points up and down. ");
        return fmt_str(p, s_mag.has_cal ? "Previous calibration kept.\r\n"
                                        : "Readings stay raw.\r\n");
    }
    mag_set_calibration(&s_mag, &cal);
    p = fmt_str(p, "Calibration done (");
    p = fmt_int(p, (int32_t)s_cal.count, 0);
    p = fmt_str(p, " samples,");
    p = fmt_coverage(p);
    p = fmt_str(p, "), now applied to readings:\r\n");
    p = fmt_calibration(p);
    return fmt_str(p, "Check: '|B|' should now stay nearly constant as you turn the module.\r\n");
}

/* Called from the main loop while calibrating. Returns ticks until the
   next thing to do. */
static TickType_t cal_tick(void) {
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(s_cal_end - now) <= 0) {
        char buf[320];
        *cal_finish(buf) = '\0';
        print_async(buf);
        return pdMS_TO_TICKS(200);
    }
    if ((int32_t)(s_cal_next_sample - now) <= 0) {
        cal_sample();   /* may wait up to one sensor period for DRDY */
        s_cal_next_sample = xTaskGetTickCount() + pdMS_TO_TICKS(CAL_SAMPLE_MS);
    }
    if ((int32_t)(s_cal_next_report - now) <= 0) {
        cal_progress();
        s_cal_next_report += pdMS_TO_TICKS(CAL_REPORT_MS);
    }
    int32_t left = (int32_t)(s_cal_next_sample - xTaskGetTickCount());
    return left > 0 ? (TickType_t)left : 0;
}

static void cmd_cal(const char *arg) {
    while (*arg == ' ') arg++;

    if (*arg == '\0') {
        char buf[160];
        char *p = buf;
        if (s_cal_active)
            p = fmt_str(p, "Calibration in progress ('cal stop' to finish).\r\n");
        *fmt_calibration(p) = '\0';
        cdc_write(buf);
        return;
    }

    if (strcmp(arg, "stop") == 0) {
        if (!s_cal_active) {
            cdc_write("Not calibrating.\r\n");
            return;
        }
        char buf[320];
        *cal_finish(buf) = '\0';
        cdc_write(buf);
        return;
    }

    if (strcmp(arg, "clear") == 0) {
        s_cal_active = 0;
        mag_set_calibration(&s_mag, NULL);
        cdc_write("Calibration cleared, readings are raw.\r\n");
        return;
    }

    if (strcmp(arg, "start") == 0 || strncmp(arg, "start ", 6) == 0) {
        long secs = CAL_DEFAULT_S;
        const char *a = arg + 5;
        while (*a == ' ') a++;
        if (*a) {
            char *end;
            secs = strtol(a, &end, 10);
            while (*end == ' ') end++;
            if (end == a || *end != '\0' || secs < 5 || secs > CAL_MAX_S) {
                cdc_write("Usage: cal start [N]   N = seconds, 5-300 (default 30)\r\n");
                return;
            }
        }
        if (!ensure_sensor())
            return;
        if (s_interval) {
            s_interval = 0;
            cdc_write("Printing readings stopped.\r\n");
        }
        mag_cal_collect_begin(&s_cal);
        s_cal_field_sum   = 0.0f;
        s_cal_active      = 1;
        TickType_t now    = xTaskGetTickCount();
        s_cal_end         = now + pdMS_TO_TICKS((uint32_t)secs * 1000U);
        s_cal_next_sample = now;
        s_cal_next_report = now + pdMS_TO_TICKS(CAL_REPORT_MS);

        char buf[320];
        char *p = fmt_str(buf, "Calibrating for ");
        p = fmt_int(p, (int32_t)secs, 0);
        p = fmt_str(p, " s. Slowly turn the module through all orientations:\r\n"
                       "  a full turn flat, a full turn on its side, then on another side,\r\n"
                       "  and slow figure-eights. Each axis should reach 100%.\r\n"
                       "Keep it away from steel objects that will not move with it.\r\n"
                       "'cal stop' finishes early.\r\n");
        *p = '\0';
        cdc_write(buf);
        return;
    }

    cdc_write("Usage: cal [start [N] | stop | clear]\r\n");
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
    if (s_cal_active) {
        cdc_write("Calibration in progress, 'cal stop' first.\r\n");
        return;
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
                  "  cal         show the calibration\r\n"
                  "  cal start [N]  calibrate: turn the module for N s (default 30)\r\n"
                  "  cal stop    finish calibrating early\r\n"
                  "  cal clear   remove the calibration (raw readings)\r\n"
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

    if (strcmp(line, "cal") == 0 || strncmp(line, "cal ", 4) == 0) {
        cmd_cal(line + 3);
        return;
    }

    if (strcmp(line, "stop") == 0) {
        cdc_write(s_interval ? "Stopped.\r\n" : "Not running.\r\n");
        s_interval = 0;
        return;
    }

    cdc_write("Unknown command. Type 'help'.\r\n");
}

/* ---- display ---- */

static void display_tick(void) {
    if (s_cal_active) {
        int cover[3];
        cal_coverage(cover);
        int32_t left_ms = (int32_t)(s_cal_end - xTaskGetTickCount()) * portTICK_PERIOD_MS;
        display_calibration(cover, (int)((left_ms + 999) / 1000));
        return;
    }
    if (!sensor_ready(0)) {
        display_message("No sensor", "check wiring");
        return;
    }
    /* Use the reading just printed to the terminal if there is a fresh one */
    if (!s_have_last ||
        (int32_t)(xTaskGetTickCount() - s_last_tick) > (int32_t)pdMS_TO_TICKS(DISPLAY_MS)) {
        mag_sample_t s;
        int err = mag_read(&s_mag, &s);
        if (err != MAG_OK) {
            display_message("Read error", mag_strerror(err));
            return;
        }
        s_last      = s;
        s_have_last = 1;
        s_last_tick = xTaskGetTickCount();
    }
    display_reading(&s_last, &s_mag);
}

/* Look for the display; set it up when one appears */
static void display_check(void) {
    if (!s_display && display_detect()) {
        display_init();
        s_display = 1;
    }
}

/* Refresh the display if due, or look for one while there is none.
   Returns ticks until the next time something is due. */
static TickType_t display_due(void) {
    int32_t left = (int32_t)(s_next_display - xTaskGetTickCount());
    if (left <= 0) {
        if (s_display) {
            display_tick();
            left = (int32_t)pdMS_TO_TICKS(DISPLAY_MS);
        } else {
            display_check();
            left = (int32_t)pdMS_TO_TICKS(s_display ? DISPLAY_MS : DISPLAY_RETRY_MS);
        }
        s_next_display = xTaskGetTickCount() + (TickType_t)left;
    }
    return (TickType_t)left;
}

/* Wait for the host to open the terminal (DTR assert), keeping the display
   going meanwhile: it works without a terminal. */
static void wait_for_host(void) {
    while (!g_cdc_connected) {
        TickType_t wait = display_due();
        vTaskDelay(wait < pdMS_TO_TICKS(100) ? wait + 1 : pdMS_TO_TICKS(100));
    }
}

void ConsoleTask(void *pvParam) {
    (void)pvParam;

    display_check();
    wait_for_host();

    cdc_write(BANNER);
    cdc_write(s_display ? "Display: connected\r\n"
                        : "Display: not found (it is picked up when connected)\r\n");
    cdc_write("Type 'help' for commands.\r\n> ");

    int last_cr = 0;   /* previous byte was '\r': swallow a following '\n' (CRLF) */

    for (;;) {
        /* Block until the next display refresh (at most 200 ms, which also
           gives the reconnect logic a chance to run), or less if a streamed
           reading or a calibration sample is due sooner. */
        TickType_t wait = display_due();
        if (s_cal_active && g_cdc_connected) {
            TickType_t next = cal_tick();
            if (next < wait)
                wait = next;
        } else if (s_interval && g_cdc_connected) {
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
                wait_for_host();
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
