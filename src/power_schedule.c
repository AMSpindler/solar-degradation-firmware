/*
 * power_schedule.c — see power_schedule.h.
 *
 * The daily ON window is [ON, OFF) in seconds-of-day (in the DS3231's timezone).
 * Outside it we deep-sleep toward the next ON time, but only SLEEP_WAKE_CHUNK_S
 * at a time: the ESP's deep-sleep timer runs off an internal RC oscillator that
 * can drift several percent, so a single 12 h sleep could miss the wake by tens
 * of minutes. Waking ~hourly to re-read the accurate DS3231 keeps the real wake
 * within one chunk's drift of the target, with no extra wiring.
 *
 * Deep sleep powers down the ESP core (~10 uA) — the big battery win. The
 * sensors stay powered (shared rail) unless a load switch is added later.
 */
#include "power_schedule.h"
#include "config.h"
#include "rtc_clock.h"

#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pwr_sched";

#define ON_SEC   ((SLEEP_ON_HOUR)  * 3600 + (SLEEP_ON_MIN)  * 60)
#define OFF_SEC  ((SLEEP_OFF_HOUR) * 3600 + (SLEEP_OFF_MIN) * 60)

/* Current second-of-day from the RTC, or -1 if unreadable / not set (year too
 * low) — in which case we never sleep, so a wrong clock can't strand the board. */
static int now_seconds(void)
{
    struct tm t;
    if (rtc_clock_read(&t) != ESP_OK) return -1;
    if (t.tm_year + 1900 < 2023)      return -1;   /* RTC not set yet */
    return t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

static bool in_on_window(int s)
{
    if (ON_SEC < OFF_SEC) return (s >= ON_SEC && s < OFF_SEC);   /* daytime window */
    return (s >= ON_SEC || s < OFF_SEC);                          /* window wraps midnight */
}

static uint32_t secs_until_on(int s)
{
    int d = ON_SEC - s;
    if (d <= 0) d += 24 * 3600;
    return (uint32_t)d;
}

/* Deep-sleep toward the next ON time (never returns). */
static void sleep_toward_on(int s)
{
    uint32_t togo  = secs_until_on(s);
    uint32_t chunk = (togo < (uint32_t)SLEEP_WAKE_CHUNK_S) ? togo : (uint32_t)SLEEP_WAKE_CHUNK_S;
    ESP_LOGW(TAG, "OFF window: deep-sleeping %lus (%lus until %02d:%02d)",
             (unsigned long)chunk, (unsigned long)togo, SLEEP_ON_HOUR, SLEEP_ON_MIN);
    esp_sleep_enable_timer_wakeup((uint64_t)chunk * 1000000ULL);
    esp_deep_sleep_start();   /* halts everything; the board reboots on wake */
}

void power_schedule_boot_gate(void)
{
#if SLEEP_SCHEDULE_ENABLE
    int s = now_seconds();
    if (s < 0) {
        ESP_LOGW(TAG, "RTC not set/readable — skipping sleep gate (running)");
        return;
    }
    if (!in_on_window(s)) {
        sleep_toward_on(s);   /* never returns */
    }
    ESP_LOGI(TAG, "ON window %02d:%02d-%02d:%02d — running",
             SLEEP_ON_HOUR, SLEEP_ON_MIN, SLEEP_OFF_HOUR, SLEEP_OFF_MIN);
#endif
}

#if SLEEP_SCHEDULE_ENABLE
static void monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        int s = now_seconds();
        if (s >= 0 && !in_on_window(s)) {
            vTaskDelay(pdMS_TO_TICKS(300));   /* let any in-flight MQTT flush */
            sleep_toward_on(s);               /* never returns */
        }
        vTaskDelay(pdMS_TO_TICKS(SLEEP_CHECK_PERIOD_S * 1000));
    }
}
#endif

void power_schedule_start_monitor(void)
{
#if SLEEP_SCHEDULE_ENABLE
    xTaskCreate(monitor_task, "pwr_sched", 3072, NULL, 5, NULL);
#endif
}