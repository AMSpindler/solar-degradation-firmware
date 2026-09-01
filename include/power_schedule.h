/*
 * power_schedule.h — RTC-based day-only operation.
 *
 * Deep-sleeps the ESP outside a daily ON window (see the SLEEP_* defines in
 * config.h) to save battery, using the DS3231 for accurate wall-clock time.
 * The ESP wakes in chunks and re-reads the RTC each time so its internal-timer
 * drift stays bounded. Implementation + rationale in power_schedule.c.
 */
#pragma once

/* Call at boot, AFTER the RTC is initialised. If the clock is in the OFF
 * window, deep-sleeps toward the next ON time and never returns (the board
 * reboots on wake). If in the ON window — or the RTC is unreadable/unset —
 * returns immediately so the firmware boots normally. No-op (returns) unless
 * SLEEP_SCHEDULE_ENABLE. */
void power_schedule_boot_gate(void);

/* Start a background task that watches the clock while running and deep-sleeps
 * once the daily OFF time arrives. No-op unless SLEEP_SCHEDULE_ENABLE. */
void power_schedule_start_monitor(void);