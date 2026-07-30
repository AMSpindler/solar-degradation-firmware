// config.h - all tunable settings for the SEN0644 reader in one place.
#pragma once

// ---- UART / wiring (ESP32-S3 -> HiLetgo TTL<->RS485 -> SEN0644) ----------
// The RS485 adapter has automatic flow control, so no DE/RE pin is needed.
// Per the adapter's reviews + datasheet: wire ESP TX -> adapter TX,
// ESP RX -> adapter RX (TX->TX, RX->RX, NOT crossed).
// Sensor side: adapter A -> sensor 485-A (yellow), adapter B -> sensor 485-B (green).
#define SEN_UART_NUM     UART_NUM_1
#define SEN_TX_PIN       17          // ESP32-S3 TX -> adapter TXD
#define SEN_RX_PIN       18          // ESP32-S3 RX <- adapter RXD
#define SEN_BAUD         9600        // SEN0644 Modbus-RTU default

// ---- Modbus ------------------------------------------------------------
#define SEN_SLAVE_ADDR   0x01        // sensor Modbus address (default 1)
#define SEN_LUX_REG      0x0002      // holding register: lux value (32-bit)
#define SEN_LUX_QTY      2           // number of 16-bit registers to read
#define SEN_LUX_DIVISOR  100.0f      // raw / 100 = lux (0.01 lux resolution)

// ---- Polling -----------------------------------------------------------
#define SEN_POLL_MS      100         // delay between reads (matches Arduino)
#define SEN_READ_TIMEOUT_MS 300      // max wait for a Modbus reply
