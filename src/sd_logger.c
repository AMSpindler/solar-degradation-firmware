/*
 * sd_logger.c — backup logging to a micro-SD card. (See sd_logger.h.)
 *
 * ============================== HOW IT WORKS ==============================
 * The SD card is wired over SPI. ESP-IDF can put a normal FAT filesystem on it
 * (the same kind a PC reads), so we just fopen()/fprintf() files like on a
 * computer. A background task pulls readings off our subscriber queue and
 * appends them to a CSV file.
 *
 * Two practical touches:
 *   - Files rotate every hour (samples_YYYYMMDD_HH.csv) so they stay small and
 *     are easy to grep/delete.
 *   - Writes are buffered (setvbuf) and only physically flushed to the card
 *     every ~10 seconds, so we're not hammering the card on every sample.
 *
 * Note: writing to flash/SD with the C library buffer means the most recent few
 * seconds live in RAM until a flush. That's fine here because Wi-Fi carries the
 * live stream; the SD file is the after-the-fact record.
 * =========================================================================
 */
#include "sd_logger.h"
#include "config.h"
#include "adc_sampler.h"
#include "sample_packet.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>                /* access, fileno, fsync */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"           /* mount a FAT filesystem on the card */
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"     /* SD card over SPI */
#include "driver/spi_common.h"

static const char *TAG = "sd_logger";

static bool           s_mounted = false;
static sdmmc_card_t  *s_card    = NULL;
static QueueHandle_t  s_queue   = NULL;

static FILE          *s_file       = NULL;
static int            s_cur_hour   = -1;   /* which hour the open file is for */
static char           s_iobuf[SD_WRITE_BUF_BYTES];  /* the write buffer */

/* ------------------------------------------------------------------------- */
/* Mount                                                                     */
/* ------------------------------------------------------------------------- */

esp_err_t sd_logger_init(void)
{
    /* Set up the SPI bus the card sits on. */
    spi_bus_config_t buscfg = {
        .mosi_io_num     = SD_PIN_MOSI,
        .miso_io_num     = SD_PIN_MISO,
        .sclk_io_num     = SD_PIN_SCLK,
        .quadwp_io_num   = -1,        /* not used for plain SPI */
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &buscfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Describe the card as a device on that bus (which pin is chip-select). */
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = SD_SPI_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    /* Mount the FAT filesystem. format_if_mount_failed=false so we never wipe a
     * card by accident; if it won't mount, we just report it. */
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "card mount failed: %s (no card? wiring?)", esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

bool sd_logger_is_mounted(void) { return s_mounted; }

/* ------------------------------------------------------------------------- */
/* File rotation                                                             */
/* ------------------------------------------------------------------------- */

/* Make sure a file for the *current hour* is open. Opens a new one (with a CSV
 * header) when the hour changes. Returns the open FILE, or NULL on failure. */
static FILE *ensure_file_open(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    if (s_file != NULL && t.tm_hour == s_cur_hour) {
        return s_file;   /* still the right hour */
    }

    /* Hour changed (or first write): close the old file, open the new one. */
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s_%04d%02d%02d_%02d.csv",
             SD_MOUNT_POINT, SD_FILE_PREFIX,
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);

    /* "a" = append, so re-opening the same hour (e.g. after a reboot) keeps data. */
    bool is_new = (access(path, F_OK) != 0);
    s_file = fopen(path, "a");
    if (s_file == NULL) {
        ESP_LOGE(TAG, "could not open %s", path);
        return NULL;
    }
    /* Use our own big buffer so the card is written in ~4 KB chunks. */
    setvbuf(s_file, s_iobuf, _IOFBF, sizeof(s_iobuf));
    if (is_new) {
        fprintf(s_file, "timestamp_us,vbus_raw,iout_raw,v_calc,i_calc,iref_raw\n");
    }
    s_cur_hour = t.tm_hour;
    ESP_LOGI(TAG, "logging to %s", path);
    return s_file;
}

/* ------------------------------------------------------------------------- */
/* Writer task                                                               */
/* ------------------------------------------------------------------------- */

static void writer_task(void *arg)
{
    (void)arg;
    int64_t last_flush_us = esp_timer_get_time();

    for (;;) {
        SamplePacket p;
        /* Wait up to 1 s for a reading; the timeout also lets us flush on time
         * even when data is sparse. */
        if (xQueueReceive(s_queue, &p, pdMS_TO_TICKS(1000)) == pdTRUE) {
            FILE *f = ensure_file_open();
            if (f != NULL) {
                float v, i;
                adc_sampler_apply_cal(&p, &v, &i);
                /* vbus_raw = PAC1951 VBUS count; aux[0] (old VOUTN) is dropped. */
                fprintf(f, "%llu,%u,%u,%.5f,%.5f,%u\n",
                        (unsigned long long)p.timestamp_us,
                        p.voltage_raw, p.current_raw,
                        v, i, p.aux_channels[1]);
            }
        }

        /* Flush to the physical card every SD_FLUSH_INTERVAL_MS. */
        int64_t now_us = esp_timer_get_time();
        if (s_file != NULL &&
            (now_us - last_flush_us) >= (int64_t)SD_FLUSH_INTERVAL_MS * 1000) {
            fflush(s_file);
            fsync(fileno(s_file));   /* push it all the way to the card */
            last_flush_us = now_us;
        }
    }
}

void sd_logger_start(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "not mounted; logger not started");
        return;
    }
    s_queue = xQueueCreate(CONSUMER_QUEUE_LEN, sizeof(SamplePacket));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "queue alloc failed; logger not started");
        return;
    }
    adc_sampler_subscribe(s_queue);
    xTaskCreatePinnedToCore(writer_task, "sd_log", 4096, NULL, PRIO_SD, NULL, CORE_NETWORK);
}
