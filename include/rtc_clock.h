/*
 * rtc_clock.h — DS3231 real-time clock over I2C.
 *
 * The DS3231 is the wall-clock reference. At boot the ESP32 system time is set
 * from it; a low-priority task re-syncs every 10 minutes and logs the drift.
 * The I2C master bus is created once in main() and shared (so the DS3231 and
 * the AT24C32 EEPROM on the same module don't both try to own the port).
 */
#pragma once

#include <time.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Add the DS3231 as a device on the already-created master bus. */
esp_err_t rtc_clock_init(i2c_master_bus_handle_t bus);

/* DS3231 -> ESP32 system time (settimeofday). */
esp_err_t rtc_clock_sync_system_time(void);

/* ESP32 system time -> DS3231 (used by a console "set time" flow). */
esp_err_t rtc_clock_set_from_system(void);

/* Read the DS3231 directly into a struct tm. */
esp_err_t rtc_clock_read(struct tm *out);

/* Start the 10-minute resync + drift-logging task. */
void rtc_clock_start_resync_task(void);
