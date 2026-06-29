/*
 * rtc_clock.c — talks to the DS3231 real-time clock chip. (See rtc_clock.h.)
 *
 * ============================== WHY WE NEED IT ==============================
 * The ESP32 has an internal clock, but it forgets the time whenever it loses
 * power. The DS3231 is a separate little chip with a coin-cell battery that
 * keeps accurate time even when everything else is off. At boot we copy the
 * time FROM the DS3231 INTO the ESP32, so every sensor reading gets a correct
 * real-world timestamp.
 *
 * We talk to it over I2C (the same 2-wire bus set up in main.c).
 *
 * One quirk: the DS3231 stores time in "BCD" (Binary-Coded Decimal). In BCD,
 * each group of 4 bits holds one decimal digit. So the number 25 is stored as
 * 0x25 (two nibbles: '2' and '5'), NOT as the binary value 25. The helpers
 * bcd2dec / dec2bcd convert between BCD and normal numbers.
 * ===========================================================================
 */
#include "rtc_clock.h"
#include "config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"           /* xTaskCreate, vTaskDelay                */
#include "esp_log.h"
#include "sys/time.h"                /* settimeofday / gettimeofday            */

static const char *TAG = "rtc_clock";

#define DS3231_REG_TIME    0x00      /* the time data starts at register 0x00 */
#define I2C_TIMEOUT_MS     1000      /* give up an I2C read/write after 1 sec  */
#define RESYNC_PERIOD_MS   (10 * 60 * 1000)  /* 10 minutes, in milliseconds    */

/* The "ticket" for the DS3231 as a device on the I2C bus. */
static i2c_master_dev_handle_t s_dev = NULL;

/* BCD -> normal: e.g. 0x25 -> 25.  (b >> 4) is the tens digit, (b & 0x0f) ones.*/
static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0f)); }
/* normal -> BCD: e.g. 25 -> 0x25. */
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

/* Register the DS3231 on the shared I2C bus (created in main.c). */
esp_err_t rtc_clock_init(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,  /* DS3231 uses a 7-bit address */
        .device_address  = DS3231_ADDR,         /* 0x68 (from config.h)        */
        .scl_speed_hz    = I2C_FREQ_HZ,         /* how fast to clock the bus   */
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
    }
    return err;
}

/*
 * Read the current time straight from the DS3231 into a standard `struct tm`
 * (the C language's calendar-time structure: year, month, day, hour, ...).
 */
esp_err_t rtc_clock_read(struct tm *out)
{
    if (s_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* How an I2C "read a register" works: first we SEND the register number we
     * want to start at (0x00), then we RECEIVE the bytes back. The DS3231
     * auto-advances, so one transaction gives us all 7 time bytes in order. */
    uint8_t reg = DS3231_REG_TIME;
    uint8_t b[7];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof(b), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* Clear the struct first, then fill it in. We mask off the top bits of some
     * bytes because the DS3231 packs status flags into them. */
    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcd2dec(b[0] & 0x7f);     /* seconds 0..59                  */
    out->tm_min  = bcd2dec(b[1] & 0x7f);     /* minutes 0..59                  */
    out->tm_hour = bcd2dec(b[2] & 0x3f);     /* hours 0..23 (we use 24h mode)  */
    out->tm_wday = (b[3] & 0x07) - 1;        /* day of week: DS3231 1..7 -> tm 0..6 */
    out->tm_mday = bcd2dec(b[4] & 0x3f);     /* day of month 1..31             */
    out->tm_mon  = bcd2dec(b[5] & 0x1f) - 1; /* month: DS3231 1..12 -> tm 0..11*/
    out->tm_year = bcd2dec(b[6]) + 100;      /* DS3231 stores 20xx; tm wants   */
                                             /* "years since 1900", so add 100 */
    out->tm_isdst = 0;                       /* no daylight-saving adjustment  */
    return ESP_OK;
}

/* Copy the DS3231's time into the ESP32's own clock. */
esp_err_t rtc_clock_sync_system_time(void)
{
    struct tm t;
    esp_err_t err = rtc_clock_read(&t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(err));
        return err;
    }
    /* mktime() turns the calendar struct into a single number: seconds since
     * Jan 1 1970 (the "epoch"), which is how computers store time internally.
     * Our timezone is left as UTC, so this is a clean conversion. */
    time_t epoch = mktime(&t);
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);  /* actually set the ESP32 system clock */
    ESP_LOGI(TAG, "system time set from DS3231: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return ESP_OK;
}

/* The reverse: write the ESP32's current time back into the DS3231. Used by the
 * `settime` console command so the battery-backed chip remembers it. */
esp_err_t rtc_clock_set_from_system(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    time_t now = time(NULL);   /* current system time as epoch seconds */
    struct tm t;
    gmtime_r(&now, &t);        /* convert epoch -> calendar fields (UTC) */

    /* Build the bytes to send. buf[0] is the starting register (0x00); the rest
     * are the time fields, converted to BCD, in the DS3231's expected order. */
    uint8_t buf[8];
    buf[0] = DS3231_REG_TIME;
    buf[1] = dec2bcd((uint8_t)t.tm_sec);
    buf[2] = dec2bcd((uint8_t)t.tm_min);
    buf[3] = dec2bcd((uint8_t)t.tm_hour);          /* 24-hour mode             */
    buf[4] = (uint8_t)(t.tm_wday + 1);             /* tm 0..6 -> DS3231 1..7   */
    buf[5] = dec2bcd((uint8_t)t.tm_mday);
    buf[6] = dec2bcd((uint8_t)(t.tm_mon + 1));     /* tm 0..11 -> 1..12        */
    buf[7] = dec2bcd((uint8_t)(t.tm_year - 100));  /* "since 1900" -> 20xx     */

    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DS3231 set from system time");
    }
    return err;
}

/*
 * A background "task" that wakes up every 10 minutes, compares the ESP32's
 * clock to the DS3231, logs how far they've drifted apart, and re-copies the
 * DS3231's (more accurate) time into the ESP32. A task is like a tiny program
 * the operating system runs alongside everything else.
 */
static void resync_task(void *arg)
{
    (void)arg;
    for (;;) {  /* loop forever */
        /* Sleep for 10 minutes. vTaskDelay hands the CPU to other work while we
         * wait — it does NOT busy-spin. pdMS_TO_TICKS converts ms to the OS's
         * internal "tick" units. */
        vTaskDelay(pdMS_TO_TICKS(RESYNC_PERIOD_MS));

        struct tm rtc_tm;
        if (rtc_clock_read(&rtc_tm) != ESP_OK) {
            ESP_LOGW(TAG, "resync: DS3231 read failed");
            continue;  /* skip this round, try again in 10 min */
        }
        time_t rtc_epoch = mktime(&rtc_tm);
        time_t sys_epoch = time(NULL);
        long drift = (long)(sys_epoch - rtc_epoch);  /* + = system ran fast */
        ESP_LOGI(TAG, "resync: system drift vs DS3231 = %ld s", drift);

        /* The DS3231 is the trusted reference; snap the system clock back to it. */
        struct timeval tv = { .tv_sec = rtc_epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }
}

/* Kick off the 10-minute resync task. The numbers are: stack size (3072 bytes
 * of memory for the task), no argument (NULL), and its priority. */
void rtc_clock_start_resync_task(void)
{
    xTaskCreate(resync_task, "rtc_resync", 3072, NULL, PRIO_RTC_RESYNC, NULL);
}
