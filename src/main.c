/*
 * main.c — CLOUDS firmware entry point (ESP32-S3, ESP-IDF).
 *
 * ============================ THE BIG PICTURE ============================
 * On a regular computer your program starts at main(). On the ESP32 using
 * ESP-IDF, it starts at app_main() instead. ESP-IDF has already set up the
 * chip, the FreeRTOS operating system, and memory before we get here.
 *
 * Unlike Arduino, there is no loop() that runs forever. Instead we:
 *   1. set things up once in app_main(), then
 *   2. let background "tasks" (mini-programs the OS runs for us) do the work.
 * When app_main() returns, those tasks keep running. 
 * The two tasks here are:
 *   1. the ADC sampling timer (grabs sensor readings 200x/second) and 
 *   2. the console (listens for what you type over USB).
 *
 * Phase A (everything needed for the HV bench test):
 *   NVS -> event loop -> I2C bus -> DS3231 clock -> ADC sampler -> console.
 *
 * Phase B (Wi-Fi/MQTT streaming and SD-card logging) will be
 * started right before console_start().
 * ========================================================================
 */
#include "config.h"          /* our pin numbers and settings (see include/) */
#include "sample_packet.h"   /* the shape of one sensor reading             */
#include "adc_sampler.h"     /* reads the voltage/current sensors           */
#include "rtc_clock.h"       /* the DS3231 real-time clock chip             */
#include "console_cmds.h"    /* the text commands you type over USB         */
#include "wifi_transport.h"  /* Phase B: send sample batches over Wi-Fi      */
#include "sd_logger.h"       /* Phase B: backup logging to the SD card       */

#include "esp_log.h"         /* ESP_LOGI(...) prints labeled debug messages  */
#include "esp_event.h"       /* "event loop" plumbing (needed by Wi-Fi later)*/
#include "nvs_flash.h"       /* NVS = a tiny built-in storage that survives  */
                             /*       power-off; we keep calibration there.  */
#include "driver/i2c_master.h" /* I2C = a 2-wire bus to talk to the DS3231   */

/* A "tag" is a short name printed at the start of each log line so you
 * can tell which part of the code a message came from. */
static const char *TAG = "main";

/*
 * Create the I2C bus that the DS3231 clock lives on.
 *
 * I2C is a simple 2-wire system:
 *   1. a data wire "SDA" and 
 *   2. a clock wire "SCL"
 * that lets the ESP32 talk to small chips. 
 * We build the bus once here, then hand it to rtc_clock so the clock chip can use it. 
 * The pin numbers come from config.h so all hardware wiring lives in one place.
 */
static i2c_master_bus_handle_t init_i2c_bus(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_PORT,        /* which I2C controller (0)      */
        .sda_io_num        = I2C_SDA_GPIO,    /* data pin                      */
        .scl_io_num        = I2C_SCL_GPIO,    /* clock pin                     */
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,               /* ignore tiny electrical noise  */
        .flags.enable_internal_pullup = true, /* gently hold wires "high"      */
    };
    /* A "handle" is like a ticket the driver gives us; 
     * we pass it back in later to say "use the thing I created earlier." */
    i2c_master_bus_handle_t bus = NULL;
    /* ESP_ERROR_CHECK aborts (and prints why) if the call fails.
     * During start-up: if the bus can't be created, we want to know loudly. */
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    return bus;
}

void app_main(void)
{
    /* 1. NVS — our tiny permanent storage. Need it for the calibration
     *    numbers (and Wi-Fi settings later). The first time the chip is used,
     *    NVS can be empty/old; if so we erase and re-create it. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    /* 2. Default event loop — a notification system ESP-IDF uses internally.
     *    Wi-Fi/MQTT (Phase B) need it; creating it now is harmless. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3. Start the I2C bus, then read the wall-clock time from the DS3231 and
     *    copy it into the ESP32's own clock. We do this BEFORE sampling so
     *    every reading gets stamped with a correct time. If the DS3231 isn't
     *    wired up yet, we keep going (can set the time later with the
     *    `settime` command). */
    i2c_master_bus_handle_t i2c_bus = init_i2c_bus();
    if (rtc_clock_init(i2c_bus) == ESP_OK) {
        if (rtc_clock_sync_system_time() != ESP_OK) {
            ESP_LOGW(TAG, "DS3231 unreadable; system time not set (use `settime`)");
        }
        rtc_clock_start_resync_task();  /* re-check the time every 10 minutes */
    } else {
        ESP_LOGW(TAG, "DS3231 init failed; continuing without RTC");
    }

    /* 4. Set up the ADC sampler. This creates the "queue" (a conveyor belt
     *    that carries readings from the sampler to whoever wants them) and
     *    loads any saved calibration from NVS. It does NOT start sampling yet. */
    ESP_ERROR_CHECK(adc_sampler_init(SAMPLE_RATE_HZ_DEFAULT));

    /* 5. Start the console: the `clouds>` prompt you type into over USB. */
    console_start();

    /* 6. Phase B — Wi-Fi sender and SD backup logger. Each subscribes to the
     *    sample stream so they get their own copy of every reading. Both are
     *    best-effort: if Wi-Fi can't connect or no SD card is present, the rest
     *    of the firmware keeps running. */
    wifi_transport_init();
    wifi_transport_start();
    if (sd_logger_init() == ESP_OK) {
        sd_logger_start();
    }

    /* 7. Now actually start sampling. From here a timer fires 200x/second on
     *    CPU core 0 and fans readings out to all subscribers. */
    ESP_ERROR_CHECK(adc_sampler_start());

    /* app_main() returns here, but the timer + console tasks keep running. */
    ESP_LOGI(TAG, "boot complete; type `help` at the prompt");
}
