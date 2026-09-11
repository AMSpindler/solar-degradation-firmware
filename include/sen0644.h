/*
 * sen0644.h — dual DFRobot SEN0644 ambient-light sensors over RS485/Modbus-RTU.
 *
 * Two sensors, each on its own ESP32 hardware UART + RS485 adapter, so both keep
 * the factory Modbus address 0x01 without colliding (see config.h SEN0644_*).
 * A background task (spawned by sen0644_init) polls both, forces them to 9600
 * baud, and recovers a sensor that drops off the bus. The latest readings are
 * cached; read them with sen0644_get_reading(). Ported from the standalone
 * Arduino project — the Modbus/CRC logic is unchanged; only the UART layer moved
 * to the ESP-IDF driver. See src/sen0644.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

/* Snapshot of both sensors' most recent poll. lux is the register value / 1000.
 *   online[i] — sensor answered detection and is being polled.
 *   valid[i]  — the last poll of this sensor succeeded (a live, fresh reading).
 *   timestamp_us — esp_timer_get_time() at the poll that produced this snapshot. */
typedef struct {
    float    lux[SEN0644_NUM_SENSORS];
    bool     online[SEN0644_NUM_SENSORS];
    bool     valid[SEN0644_NUM_SENSORS];
    uint64_t timestamp_us;
} sen0644_reading_t;

/* Install both UARTs and spawn the poll/recovery task. Returns immediately
 * (detection and any power-cycle wait happen inside the task), so it never
 * blocks app_main boot. */
esp_err_t sen0644_init(void);

/* Copy the cached latest snapshot into *out (thread-safe). Returns ESP_OK once
 * the driver is initialised, ESP_ERR_INVALID_STATE before sen0644_init(). */
esp_err_t sen0644_get_reading(sen0644_reading_t *out);
