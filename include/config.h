/*
 * config.h — Single source of truth for pin assignments and tunables.
 *
 * Every GPIO and every knob the hardware/bench setup depends on lives here so
 * wiring changes happen in one place. Hardware-specifics still to be confirmed
 * on the bench are marked "CONFIRM".
 *
 * Board: ESP32-S3-DevKitC-N8R2 (8 MB flash, 2 MB quad PSRAM), ESP-IDF.
 *
 * Everything below is a `#define`: a named constant. Before the code compiles,
 * the compiler literally replaces each name with its value everywhere it's used.
 * So changing a pin here changes it across the whole project. Names marked
 * "CONFIRM" are best guesses to verify against your actual wiring.
 */
#pragma once

#include "esp_adc/adc_oneshot.h"
#include "driver/i2c_types.h"

/* ----------------------------------------------------------------------------
 * Device identity
 * ------------------------------------------------------------------------- */
#define DEVICE_ID                0x534F4C31u   /* "SOL1" — change per unit     */

/* ----------------------------------------------------------------------------
 * Sampling
 * ------------------------------------------------------------------------- */
#define SAMPLE_RATE_HZ_DEFAULT   200           /* valid range 100..500 Hz      */
#define SAMPLE_RATE_HZ_MIN       100
#define SAMPLE_RATE_HZ_MAX       500
#define SAMPLE_QUEUE_LEN         1024          /* console plot queue (~5 s)    */
#define BATCH_SAMPLES            100           /* samples per UDP/MQTT/SD batch */

/* Fan-out: the sampler copies each reading into several independent "consumer"
 * queues (console plot, Wi-Fi sender, SD logger) so they never steal data from
 * each other. Each consumer gets its own queue of CONSUMER_QUEUE_LEN packets. */
#define MAX_SAMPLE_SUBSCRIBERS   4
#define CONSUMER_QUEUE_LEN       512           /* ~2.5 s buffer @200 Hz        */

/* ----------------------------------------------------------------------------
 * ADC  — ADC1 ONLY. ADC2 shares hardware with WiFi on the ESP32-S3 and its
 *        reads fail while WiFi is active, so all sampling stays on ADC1.
 *        ESP32-S3 ADC1 = GPIO1..GPIO10 (ADC1_CH0..CH9).
 *
 * AMC1311 voltage output is DIFFERENTIAL: VOUTP and VOUTN are read on two
 * channels and the firmware computes (VOUTP - VOUTN). ACS724 current output is
 * single-ended.
 * ------------------------------------------------------------------------- */
#define ADC_UNIT                 ADC_UNIT_1
#define ADC_VOLTAGE_P_CHANNEL    ADC_CHANNEL_0 /* GPIO1 — AMC1311 VOUTP (+)     */
#define ADC_VOLTAGE_N_CHANNEL    ADC_CHANNEL_1 /* GPIO2 — AMC1311 VOUTN (-)     */
#define ADC_CURRENT_CHANNEL      ADC_CHANNEL_2 /* GPIO3 — ACS724 Vout           */
#define ADC_AUX_CHANNEL          ADC_CHANNEL_3 /* GPIO4 — spare (CONFIRM)       */
#define ADC_ATTEN                ADC_ATTEN_DB_12 /* full-scale ~0..3.1 V        */
#define ADC_BITWIDTH             ADC_BITWIDTH_12 /* 0..4095                     */

#define ADC_READ_ONCE_AVG_N      500           /* samples averaged per cal point */

/* ----------------------------------------------------------------------------
 * I2C — DS3231 RTC + AT24C32 EEPROM module (shared bus, created in main).
 * ------------------------------------------------------------------------- */
#define I2C_PORT                 I2C_NUM_0
#define I2C_SDA_GPIO             8             /* CONFIRM                       */
#define I2C_SCL_GPIO             9             /* CONFIRM                       */
#define I2C_FREQ_HZ              400000
#define DS3231_ADDR              0x68
#define AT24C32_ADDR             0x57

/* ----------------------------------------------------------------------------
 * SD card over SPI  (Phase B backup logging).
 * SD pins MUST NOT collide with the ADC1 channels above (GPIO1..GPIO4).
 * ------------------------------------------------------------------------- */
#define SD_SPI_HOST              SPI2_HOST
#define SD_PIN_MOSI              11            /* CONFIRM                       */
#define SD_PIN_MISO              13            /* CONFIRM                       */
#define SD_PIN_SCLK              12            /* CONFIRM                       */
#define SD_PIN_CS                10            /* CONFIRM                       */
#define SD_MOUNT_POINT           "/sdcard"
#define SD_FILE_PREFIX           "samples"     /* -> samples_YYYYMMDD_HH.csv    */
#define SD_WRITE_BUF_BYTES       4096          /* buffered writes (setvbuf)     */
#define SD_FLUSH_INTERVAL_MS     10000         /* flush to card every 10 s      */

/* ----------------------------------------------------------------------------
 * WiFi  (Phase B). Fill in your lab network credentials.
 * ------------------------------------------------------------------------- */
#define WIFI_SSID                "your-ssid"               /* FILL             */
#define WIFI_PASS                "your-pass"               /* FILL             */
/* Reconnect backoff: wait grows 1s -> 2s -> ... capped at 30s between tries.  */
#define WIFI_RECONNECT_MIN_MS    1000
#define WIFI_RECONNECT_MAX_MS    30000

/* ----------------------------------------------------------------------------
 * Transport selection (Phase B). Turn each on (1) or off (0). You can run both;
 * UDP works with no broker, MQTT needs Mosquitto running on the lab PC AND the
 * esp-mqtt library vendored into components/ (see README).
 * ------------------------------------------------------------------------- */
#define TRANSPORT_USE_UDP        1
#define TRANSPORT_USE_MQTT       1             /* esp-mqtt vendored in components/mqtt */

/* --- UDP: the ESP32 sends sample batches straight to this PC + port. --- */
#define UDP_DEST_IP              "192.168.1.10"  /* FILL: your lab PC's IP      */
#define UDP_DEST_PORT            9000

/* --- MQTT: publish to a Mosquitto broker. Topic includes the device id. --- */
#define MQTT_BROKER_URI          "mqtt://192.168.1.10:1883" /* FILL            */
#define MQTT_TOPIC_FMT           "clouds/%08lX/samples"     /* device_id        */
#define MQTT_QOS                 1

/* ----------------------------------------------------------------------------
 * Task placement (FreeRTOS).  Core 0 = real-time sampling, Core 1 = I/O.
 * ------------------------------------------------------------------------- */
#define CORE_SAMPLER             0
#define CORE_NETWORK             1
#define PRIO_RTC_RESYNC          2
#define PRIO_WIFI                5
#define PRIO_SD                  3

/* ----------------------------------------------------------------------------
 * Calibration storage (NVS).
 * ------------------------------------------------------------------------- */
#define NVS_CAL_NAMESPACE        "cal"
