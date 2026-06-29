/*
 * rtc_clock.h — the "menu" of functions for the DS3231 real-time clock.
 *
 * The DS3231 is a battery-backed clock chip that keeps accurate time even when
 * the board is off. We treat it as the source of truth: at boot we copy its
 * time into the ESP32, and a background task re-checks every 10 minutes.
 *
 * The I2C bus is created once in main.c and shared with this module (so the
 * DS3231 and the AT24C32 memory chip on the same little board don't both try to
 * own the bus). The actual code is in rtc_clock.c.
 */
#pragma once

#include <time.h>                    /* struct tm = a calendar date/time      */
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Register the DS3231 as a device on the already-created I2C bus. */
esp_err_t rtc_clock_init(i2c_master_bus_handle_t bus);

/* Copy the time DS3231 -> ESP32 system clock (done at boot). */
esp_err_t rtc_clock_sync_system_time(void);

/* Copy the time ESP32 system clock -> DS3231 (used by the `settime` command). */
esp_err_t rtc_clock_set_from_system(void);

/* Read the DS3231's current time into `out`. */
esp_err_t rtc_clock_read(struct tm *out);

/* Start the background task that re-syncs + logs drift every 10 minutes. */
void rtc_clock_start_resync_task(void);
