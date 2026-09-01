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
#define SAMPLE_RATE_HZ_DEFAULT   500           /* valid range 100..500 Hz      */
#define SAMPLE_RATE_HZ_MIN       1
#define SAMPLE_RATE_HZ_MAX       500
#define SAMPLE_QUEUE_LEN         1024          /* console plot queue (~5 s)    */
#define BATCH_SAMPLES            100           /* samples per UDP/MQTT/SD batch */

/* Fan-out: the sampler copies each reading into several independent "consumer"
 * queues (console plot, Wi-Fi sender, SD logger) so they never steal data from
 * each other. Each consumer gets its own queue of CONSUMER_QUEUE_LEN packets. */
#define MAX_SAMPLE_SUBSCRIBERS   4
#define CONSUMER_QUEUE_LEN       512           /* ~2.5 s buffer @200 Hz        */

/* ----------------------------------------------------------------------------
 * Subsystem enable — turn OFF for clean USB-only bench testing (no Wi-Fi/SD
 * error spam in the console). Set back to 1 to use the network / SD path.
 * ------------------------------------------------------------------------- */
#define ENABLE_WIFI              1             /* Wi-Fi + UDP/MQTT transport   */
#define ENABLE_SD                0             /* SD card backup logging       */
/* PAC1951 voltage-over-I2C. OFF = the sampler skips the PAC read entirely and
 * reports VBUS = 0 — use this until the PAC1951/ISO1540 are wired, so a missing
 * chip can't spam "I2C transaction timeout". Turn ON once the isolated I2C bus
 * is in place and you want real voltage. */
#define ENABLE_PAC1951           1             /* voltage via PAC1951 (I2C)    */

/* ----------------------------------------------------------------------------
 * Scheduled sleep — RTC-based day-only operation to save battery. Outside the
 * daily ON window the ESP deep-sleeps (core ~10 uA). It wakes every
 * SLEEP_WAKE_CHUNK_S and re-reads the DS3231, so the wake stays tied to the
 * accurate RTC despite the ESP's internal-timer drift.
 *   TIMES ARE IN THE DS3231's TIMEZONE — set the RTC to LOCAL time (`settime`),
 *   or offset these hours. Deep sleep powers down the ESP only; the sensors stay
 *   powered unless a load switch is added. Set ENABLE 0 for bench work (else the
 *   board sleeps whenever the clock is outside the window). Default 7:30-19:30.
 * ------------------------------------------------------------------------- */
#define SLEEP_SCHEDULE_ENABLE    0
#define SLEEP_ON_HOUR            21     /* start running at 07:30 local (11 UTC)          */
#define SLEEP_ON_MIN             5
#define SLEEP_OFF_HOUR           21     /* sleep at 19:30 local (23 UTC)         */
#define SLEEP_OFF_MIN            30
#define SLEEP_WAKE_CHUNK_S       3600   /* re-check the real clock at least hourly*/
#define SLEEP_CHECK_PERIOD_S     10     /* how often (s) to check for OFF while awake */

/* ----------------------------------------------------------------------------
 * ADC  — ADC1 ONLY, and now CURRENT ONLY. ADC2 shares hardware with WiFi on the
 *        ESP32-S3 and its reads fail while WiFi is active, so all sampling stays
 *        on ADC1. ESP32-S3 ADC1 = GPIO1..GPIO10 (ADC1_CH0..CH9).
 *
 * VOLTAGE is no longer read here. It moved off the ESP32 ADC (the old AMC1311
 * analog path) onto the PAC1951 power monitor, which digitizes the bus voltage
 * itself and is read over I2C through the ISO1540 isolator (see the PAC1951
 * block below). Only the current sensor still uses the ADC.
 *
 * The HSTS016L current output is DIFFERENTIAL — Vout/Vref (its white reference
 * lead). The firmware reads the pair on two channels and uses the difference
 * (Vout - Vref). The HSTS016L runs on 5 V; its 2.5 V±0.625 V output (≈31.25 mV/A
 * on the 20 A model) sits within the 3.3 V ADC range, so it wires in directly.
 * ------------------------------------------------------------------------- */
#define ADC_UNIT                 ADC_UNIT_1
#define ADC_CURRENT_CHANNEL      ADC_CHANNEL_2 /* HSTS016L Vout (yellow) */
#define ADC_CURRENT_REF_CHANNEL  ADC_CHANNEL_7 /* HSTS016L Vref (white)  */
#define ADC_ATTEN                ADC_ATTEN_DB_12 /* full-scale ~0..3.1 V        */
#define ADC_BITWIDTH             ADC_BITWIDTH_12 /* 0..4095                     */

#define ADC_READ_ONCE_AVG_N      500           /* samples averaged per cal point */

/* Current sensor sensitivity, used to convert to amps when the current channel
 * is NOT two-point calibrated (physics: amps = (Vout-Vref mV)/sens/turns).
 * 20 A HSTS016L = 0.625 V / 20 A = 31.25 mV/A. */
#define CURRENT_SENS_MV_PER_A    31.25f
#define CURRENT_TURNS_DEFAULT    10             /* wire passes through sensor hole */

/* ADC oversampling: reads averaged per sample to cut noise (1 = off). Each step
 * multiplies ADC work per sample. At 200 Hz, ~8 is the safe ceiling (4 channels
 * x 8 reads fits the 5 ms window); higher starves the CPU and trips the task
 * watchdog. For MORE averaging, lower SAMPLE_RATE_HZ_DEFAULT (bench sweeps don't
 * need 200 Hz). Change live with the `avg` command. */
#define ADC_OVERSAMPLE_DEFAULT   4

/* ----------------------------------------------------------------------------
 * I2C — shared bus (created in main): DS3231 RTC + AT24C32 EEPROM + PAC1951.
 * Pins GPIO4/GPIO5 verified working with the PAC on the bench (was 8/9; GPIO8
 * is now free for the current-sensor ADC).
 * ------------------------------------------------------------------------- */
#define I2C_PORT                 I2C_NUM_0
#define I2C_SDA_GPIO             4             /* verified with PAC1951         */
#define I2C_SCL_GPIO             5             /* verified with PAC1951         */
#define I2C_FREQ_HZ              400000
#define DS3231_ADDR              0x68
#define AT24C32_ADDR             0x57

/* ----------------------------------------------------------------------------
 * PAC1951 voltage monitor — the AMC1311 replacement. Sits on the 600 V side and
 * digitizes the bus voltage (VBUS) with its own 16-bit ADC; the ESP32 reads it
 * over the SAME I2C bus above, through an ISO1540 isolator that carries SDA/SCL
 * across the HV barrier (the ISO1540 needs no firmware — it is transparent).
 *
 * Address is set by the resistor on the PAC's ADDRSEL pin (range 0x10..0x1F);
 * 0x10 = ADDRSEL tied to GND. No collision with the DS3231 (0x68).
 *
 * The PAC's VBUS input maxes at 32 V, so an external divider brings the 600 V
 * bus down to <=32 V. Firmware does NOT hardcode that divider ratio: it reports
 * the raw 16-bit VBUS register and the two-point `cal v` slope absorbs the
 * divider (see adc_sampler_apply_cal). FSR/full-scale below are for reference.
 * ------------------------------------------------------------------------- */
#define PAC1951_ADDR             0x10          /* ADDRSEL->GND. CONFIRM strap   */
#define PAC1951_REG_REFRESH      0x00          /* Send Byte: refresh + reset acc*/
#define PAC1951_REG_VBUS         0x07          /* VBUS1 result (16-bit, MSB 1st)*/
#define PAC1951_REG_VBUS_AVG     0x0F          /* VBUS1 rolling 8-sample average */
#define PAC1951_REG_REFRESH_V    0x1F          /* Send Byte: refresh, keep acc  */
#define PAC1951_VBUS_FSR_V       32.0f         /* default VBUS full-scale range  */
#define PAC1951_VBUS_FULL_SCALE  65536.0f      /* 16-bit unipolar counts         */
/* Stream the PAC's 8-sample rolling average (VBUS_AVG, 0x0F) instead of the
 * instantaneous VBUS (0x07). 1 = ~sqrt(8) (~2.8x) less voltage noise, but ~8 ms
 * response so high-rate samples overlap; 0 = raw instantaneous. Affects only the
 * streamed sample — calibration already uses the averaged register. */
#define PAC1951_STREAM_AVG       0

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
#define WIFI_SSID                "eduroam"                 /* wifi name  */
#define WIFI_PASS                "YOUR_WIFI_PASSWORD"       /* wifi password  */

/* WPA2-Enterprise (campus networks: eduroam / MWireless). Set to 1 and fill in
 * your UMich credentials to use 802.1X login instead of a simple password.
 * Leave 0 for a normal password network (hotspot/home). WIFI_PASS is ignored
 * when this is on. DO NOT commit real credentials to git. Note: even once
 * connected, campus client-isolation may block the ESP32 from reaching a broker
 * on your laptop. */
#define WIFI_ENTERPRISE          1
/* MWireless: use your uniqname (no @umich.edu). eduroam: use uniqname@umich.edu. */
#define EAP_IDENTITY             "uniqname@umich.edu"    /* umich email */
#define EAP_USERNAME             "uniqname@umich.edu"    /* umich email */
#define EAP_PASSWORD             "YOUR_UMICH_PASSWORD"      /* umich password */

/* Reconnect backoff: wait grows 1s -> 2s -> ... capped at 30s between tries.  */
#define WIFI_RECONNECT_MIN_MS    1000
#define WIFI_RECONNECT_MAX_MS    30000

/* ----------------------------------------------------------------------------
 * Transport selection (Phase B). Turn each on (1) or off (0). You can run both;
 * UDP works with no broker, MQTT needs Mosquitto running on the lab PC AND the
 * esp-mqtt library vendored into components/ (see README).
 * ------------------------------------------------------------------------- */
/* BATTERY TEST: MQTT (port 1883) is blocked by the campus firewall and UDP
 * needs the laptop on, so both are OFF here; the ESP reports over HTTP to
 * ThingSpeak instead (see the THINGSPEAK block below). Set both back to 1 for
 * normal broker/UDP operation. */
#define TRANSPORT_USE_UDP        0
#define TRANSPORT_USE_MQTT       1             /* esp-mqtt vendored in components/mqtt */

/* --- UDP: the ESP32 sends sample batches straight to this PC + port. --- */
#define UDP_DEST_IP              "x"  /* FILL: your lab PC's IP      */
#define UDP_DEST_PORT            9000

/* --- MQTT: publish to a Mosquitto broker. Topic includes the device id. --- */
/* BATTERY TEST: a free always-on PUBLIC broker so no PC/Pi of ours has to stay
 * running. Reached outbound, so eduroam client-isolation doesn't block it.
 * Comment this and uncomment the normal broker below to switch back. */
#define MQTT_BROKER_URI          "mqtt://35.3.248.216:1883"
#define MQTT_TOPIC_FMT           "clouds/%08lX/samples"     /* device_id        */
#define MQTT_QOS                 1

/* Online/offline status ("presence") topic. The board publishes RETAINED
 * "online" when it connects; it also arms an MQTT Last Will so the BROKER
 * publishes RETAINED "offline" automatically if the board drops (Wi-Fi loss,
 * power loss, crash). Watch it from the PC with no USB needed:
 *     mosquitto_sub -t 'clouds/+/status' -v
 * Retained = a client that subscribes late still sees the last known state. */
#define MQTT_STATUS_TOPIC_FMT    "clouds/%08lX/status"      /* device_id        */
#define MQTT_STATUS_ONLINE       "online"
#define MQTT_STATUS_OFFLINE      "offline"

/* WiFi/MQTT round-trip self-test. When on, the firmware publishes a heartbeat
 * counter to WIFI_TEST_TOPIC_OUT once a second — RETAINED, so the broker keeps
 * the LAST value. For the overnight battery test, read the last counter the ESP
 * sent before it died (no logger of ours needs to run):
 *     mosquitto_sub -h test.mosquitto.org -t clouds/solarbatt-9f3a/counter -v -C 1
 *     mosquitto_pub -h test.mosquitto.org -t clouds/solarbatt-9f3a/in  -m 42
 * Topics are unique so they don't collide on the shared public broker.
 * Set to 0 for production (no extra traffic). */
#define WIFI_TEST_ENABLE         1
#define WIFI_TEST_TOPIC_OUT      "clouds/solarbatt-9f3a/counter" /* ESP -> counter */
#define WIFI_TEST_TOPIC_IN       "clouds/solarbatt-9f3a/in"      /* PC -> ESP msgs */
#define WIFI_TEST_PERIOD_MS      1000                        /* heartbeat cadence */

/* ----------------------------------------------------------------------------
 * ThingSpeak HTTP heartbeat (battery test). MQTT 1883 is firewall-blocked on
 * campus, but HTTP (port 80) is not — so the ESP GETs a counter to ThingSpeak,
 * which timestamps and charts it. Watch the chart from anywhere; when it stops
 * updating, that timestamp is when the battery died.
 *   1. Make a free channel at thingspeak.com, enable Field 1.
 *   2. Paste its Write API Key below.
 * Free tier requires >= 15 s between updates. Set THINGSPEAK_ENABLE 0 for
 * normal (non-HTTP) operation.
 * ------------------------------------------------------------------------- */
#define THINGSPEAK_ENABLE        0
#define THINGSPEAK_HOST          "api.thingspeak.com"
#define THINGSPEAK_API_KEY       "YOUR_WRITE_KEY"    /* channel Write API Key   */
#define THINGSPEAK_PERIOD_MS     20000               /* >= 15000 on the free tier */

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
