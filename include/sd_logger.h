/*
 * sd_logger.h — the "menu" for backup logging to a micro-SD card.
 *
 * The SD card is the safety net: even if Wi-Fi drops, every reading is still
 * written here. It saves human-readable CSV files, one per hour
 * (samples_YYYYMMDD_HH.csv), so they're easy to find and inspect. The card is
 * wired over SPI (pins in config.h). Code lives in sd_logger.c.
 */
#pragma once

#include "esp_err.h"

/* Mount the SD card. Returns an error (and logs why) if no card is found —
 * the rest of the firmware keeps running without SD in that case. */
esp_err_t sd_logger_init(void);

/* True if the card mounted successfully. */
bool sd_logger_is_mounted(void);

/* Subscribe to the sample stream and start the background writer task.
 * Only call this if sd_logger_init() succeeded. */
void sd_logger_start(void);
