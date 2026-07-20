/*
 * pac1951.h — the "menu" of functions for the PAC1951 voltage monitor.
 *
 * The PAC1951 replaces the old AMC1311 analog voltage path. It is a Microchip
 * power monitor with its own 16-bit ADC: it digitizes the bus voltage (VBUS)
 * and we read that number over I2C. It lives on the 600 V high-voltage side, so
 * the I2C wires reach it through an ISO1540 isolator (which needs no code — it
 * just carries SDA/SCL across the isolation barrier).
 *
 * Like the DS3231, the PAC shares the one I2C bus created in main.c. The actual
 * code is in pac1951.c.
 *
 * HOW A READ WORKS: the PAC's readable VBUS register only updates when we send a
 * "REFRESH" command; between refreshes it holds the last converted value. The
 * sampler pipelines this — read the value refreshed last tick, then ask for a
 * fresh one — so the 200 Hz loop never has to block waiting for a conversion.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Register the PAC1951 as a device on the already-created I2C bus, and prime it
 * with one REFRESH so the first VBUS read returns real data. */
esp_err_t pac1951_init(i2c_master_bus_handle_t bus);

/* Ask the PAC to refresh its readable registers WITHOUT resetting its internal
 * energy accumulators (REFRESH_V). Cheap 1-byte write; call once per sample so
 * the NEXT read returns a fresh conversion. */
esp_err_t pac1951_refresh_v(void);

/* Read the most-recent VBUS conversion as a raw 16-bit count (0..65535).
 * Multiply by the divider/FSR (or run `cal v`) to get real volts. */
esp_err_t pac1951_read_vbus_raw(uint16_t *raw);

/* Read the PAC's on-chip rolling average of the last 8 VBUS conversions — a
 * lower-noise value used by the two-point voltage calibration. */
esp_err_t pac1951_read_vbus_avg_raw(uint16_t *raw);
