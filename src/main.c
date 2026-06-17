/*
 * main.c — CLOUDS firmware entry point (ESP32-S3, ESP-IDF).
 *
 * Phase A (lab-critical path for the HV staircase test):
 *   NVS -> event loop -> I2C bus -> DS3231 (sync system time) ->
 *   ADC sampler (loads calibration) -> console (REPL) -> start sampling.
 *
 * Phase B (network/SD) initialization will slot in before console_start():
 *   wifi_transport_init(); sd_logger_init(); + create their Core-1 tasks.
 */
#include "config.h"
#include "sample_packet.h"
#include "adc_sampler.h"
#include "rtc_clock.h"
#include "console_cmds.h"

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"

static const char *TAG = "main";

static i2c_master_bus_handle_t init_i2c_bus(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_PORT,
        .sda_io_num        = I2C_SDA_GPIO,
        .scl_io_num        = I2C_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    return bus;
}

void app_main(void)
{
    /* 1. NVS — required for calibration storage (and WiFi in Phase B). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    /* 2. Default event loop (used by WiFi/MQTT in Phase B; harmless now). */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3. Shared I2C bus, then DS3231 -> system time before any timestamping. */
    i2c_master_bus_handle_t i2c_bus = init_i2c_bus();
    if (rtc_clock_init(i2c_bus) == ESP_OK) {
        if (rtc_clock_sync_system_time() != ESP_OK) {
            ESP_LOGW(TAG, "DS3231 unreadable; system time not set (use `settime`)");
        }
        rtc_clock_start_resync_task();
    } else {
        ESP_LOGW(TAG, "DS3231 init failed; continuing without RTC");
    }

    /* 4. ADC sampler — creates the queue, ADC unit, loads calibration. */
    ESP_ERROR_CHECK(adc_sampler_init(SAMPLE_RATE_HZ_DEFAULT));

    /* 5. Console (REPL over USB-Serial-JTAG). */
    console_start();

    /* 6. Begin sampling. The esp_timer task is pinned to CPU0 via sdkconfig. */
    ESP_ERROR_CHECK(adc_sampler_start());

    ESP_LOGI(TAG, "boot complete; type `help` at the prompt");
}
