/*
 * rtc_clock.c — see rtc_clock.h.
 *
 * DS3231 timekeeping registers (auto-incrementing from 0x00):
 *   0x00 sec  0x01 min  0x02 hour  0x03 dow  0x04 date  0x05 month  0x06 year
 * All BCD. Month bit7 is a century flag; we operate purely in the 2000s.
 */
#include "rtc_clock.h"
#include "config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sys/time.h"

static const char *TAG = "rtc_clock";

#define DS3231_REG_TIME    0x00
#define I2C_TIMEOUT_MS     1000
#define RESYNC_PERIOD_MS   (10 * 60 * 1000)

static i2c_master_dev_handle_t s_dev = NULL;

static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0f)); }
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

esp_err_t rtc_clock_init(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = DS3231_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t rtc_clock_read(struct tm *out)
{
    if (s_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t reg = DS3231_REG_TIME;
    uint8_t b[7];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof(b), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcd2dec(b[0] & 0x7f);
    out->tm_min  = bcd2dec(b[1] & 0x7f);
    out->tm_hour = bcd2dec(b[2] & 0x3f);          /* assumes 24-hour mode */
    out->tm_wday = (b[3] & 0x07) - 1;             /* DS3231 1..7 -> tm 0..6 */
    out->tm_mday = bcd2dec(b[4] & 0x3f);
    out->tm_mon  = bcd2dec(b[5] & 0x1f) - 1;      /* 1..12 -> 0..11 */
    out->tm_year = bcd2dec(b[6]) + 100;           /* 20xx -> years since 1900 */
    out->tm_isdst = 0;
    return ESP_OK;
}

esp_err_t rtc_clock_sync_system_time(void)
{
    struct tm t;
    esp_err_t err = rtc_clock_read(&t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(err));
        return err;
    }
    /* TZ is unset (UTC) so mktime() treats the fields as UTC. */
    time_t epoch = mktime(&t);
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "system time set from DS3231: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return ESP_OK;
}

esp_err_t rtc_clock_set_from_system(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);

    uint8_t buf[8];
    buf[0] = DS3231_REG_TIME;
    buf[1] = dec2bcd((uint8_t)t.tm_sec);
    buf[2] = dec2bcd((uint8_t)t.tm_min);
    buf[3] = dec2bcd((uint8_t)t.tm_hour);         /* 24-hour mode */
    buf[4] = (uint8_t)(t.tm_wday + 1);
    buf[5] = dec2bcd((uint8_t)t.tm_mday);
    buf[6] = dec2bcd((uint8_t)(t.tm_mon + 1));
    buf[7] = dec2bcd((uint8_t)(t.tm_year - 100)); /* years since 1900 -> 20xx */

    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DS3231 set from system time");
    }
    return err;
}

static void resync_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RESYNC_PERIOD_MS));

        struct tm rtc_tm;
        if (rtc_clock_read(&rtc_tm) != ESP_OK) {
            ESP_LOGW(TAG, "resync: DS3231 read failed");
            continue;
        }
        time_t rtc_epoch = mktime(&rtc_tm);
        time_t sys_epoch = time(NULL);
        long drift = (long)(sys_epoch - rtc_epoch);
        ESP_LOGI(TAG, "resync: system drift vs DS3231 = %ld s", drift);

        /* DS3231 is the reference: correct the system clock to match it. */
        struct timeval tv = { .tv_sec = rtc_epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }
}

void rtc_clock_start_resync_task(void)
{
    xTaskCreate(resync_task, "rtc_resync", 3072, NULL, PRIO_RTC_RESYNC, NULL);
}
