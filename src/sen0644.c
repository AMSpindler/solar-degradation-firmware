/*
 * sen0644.c — dual SEN0644 lux sensor driver (RS485/Modbus-RTU over UART).
 * See sen0644.h. Ported from the standalone Arduino logger: the CRC, the fixed
 * Modbus frames, baud detection, and recovery are the same; HardwareSerial was
 * replaced with the ESP-IDF UART driver.
 *
 *   Sensor #1 -> UART1, RX SEN0644_UART1_RX_GPIO, TX SEN0644_UART1_TX_GPIO
 *   Sensor #2 -> UART2, RX SEN0644_UART2_RX_GPIO, TX SEN0644_UART2_TX_GPIO
 *
 * The two ~18 ms Modbus reads run in this file's own task on the network core,
 * never in the 500 Hz sample timer.
 */
#include "sen0644.h"
#include "config.h"

#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sen0644";

#define UART_RX_BUF 256   /* must exceed the HW FIFO (129); TX stays unbuffered */

/* Fixed Modbus-RTU request frames, CRC-16 baked in (address 0x01):
 *   READ_LUX     : read reg 0x0002, 2 words -> 32-bit lux, /1000
 *   SET_LEVEL_1  : write reg 0x0046 = 1 (acquisition level)
 *   SET_BAUD_9600: write reg 0x0065 = 3 (9600; takes effect after power cycle) */
static const uint8_t READ_LUX[]      = {0x01, 0x03, 0x00, 0x02, 0x00, 0x02, 0x65, 0xCB};
static const uint8_t SET_LEVEL_1[]   = {0x01, 0x06, 0x00, 0x46, 0x00, 0x01, 0xA9, 0xDF};
static const uint8_t SET_BAUD_9600[] = {0x01, 0x06, 0x00, 0x65, 0x00, 0x03, 0xD9, 0xD4};

static const uint32_t CANDIDATE_BAUDS[] = {9600, 57600, 4800, 19200, 38400, 115200, 2400};
#define NUM_BAUDS (sizeof(CANDIDATE_BAUDS) / sizeof(CANDIDATE_BAUDS[0]))

typedef struct {
    uart_port_t uart_num;
    const char *name;
    int         rx_pin;
    int         tx_pin;
    uint32_t    baud;          /* detected baud, 0 while unknown */
    bool        online;
    uint16_t    consec_fail;   /* consecutive failed reads while online */
    int64_t     last_attempt_us;
} sensor_t;

static sensor_t s_sensors[SEN0644_NUM_SENSORS] = {
    { SEN0644_UART1_NUM, "sensor1", SEN0644_UART1_RX_GPIO, SEN0644_UART1_TX_GPIO, 0, false, 0, 0 },
    { SEN0644_UART2_NUM, "sensor2", SEN0644_UART2_RX_GPIO, SEN0644_UART2_TX_GPIO, 0, false, 0, 0 },
};

static sen0644_reading_t s_latest;
static SemaphoreHandle_t s_lock;

/* ---- Modbus CRC-16 (unchanged from the Arduino source) ------------------ */
static uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

/* One request/response transaction. Returns bytes received. */
static int send_and_receive(sensor_t *s, const uint8_t *frame, size_t frame_len,
                            uint8_t *resp, size_t resp_len)
{
    uart_flush_input(s->uart_num);
    uart_write_bytes(s->uart_num, frame, frame_len);
    uart_wait_tx_done(s->uart_num, pdMS_TO_TICKS(50));
    int n = uart_read_bytes(s->uart_num, resp, resp_len,
                            pdMS_TO_TICKS(SEN0644_RESP_TIMEOUT_MS));
    return n < 0 ? 0 : n;
}

static bool read_lux_raw(sensor_t *s, uint32_t *raw)
{
    uint8_t resp[9];
    int n = send_and_receive(s, READ_LUX, sizeof(READ_LUX), resp, sizeof(resp));
    if (n != 9 || resp[0] != 0x01 || resp[1] != 0x03 || resp[2] != 0x04) return false;
    uint16_t crc = crc16_modbus(resp, 7);
    if (resp[7] != (crc & 0xFF) || resp[8] != (crc >> 8)) return false;
    *raw = ((uint32_t)resp[3] << 24) | ((uint32_t)resp[4] << 16) |
           ((uint32_t)resp[5] << 8) | resp[6];
    return true;
}

static bool sensor_responds(sensor_t *s)
{
    uint32_t dummy;
    return read_lux_raw(s, &dummy);
}

static uint32_t detect_baud(sensor_t *s)
{
    for (size_t i = 0; i < NUM_BAUDS; i++) {
        uart_set_baudrate(s->uart_num, CANDIDATE_BAUDS[i]);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (sensor_responds(s)) return CANDIDATE_BAUDS[i];
        if (sensor_responds(s)) return CANDIDATE_BAUDS[i];   /* one retry per baud */
    }
    return 0;
}

static bool write_and_confirm(sensor_t *s, const uint8_t *frame)
{
    uint8_t resp[8];
    int n = send_and_receive(s, frame, 8, resp, sizeof(resp));
    return n == 8 && memcmp(resp, frame, 8) == 0;
}

/* Bring one sensor up: find it, set level 1, force baud back to 9600. Safe to
 * call again at runtime to recover a sensor that dropped off the bus. */
static void init_sensor(sensor_t *s)
{
    s->last_attempt_us = esp_timer_get_time();
    s->consec_fail = 0;

    uart_set_baudrate(s->uart_num, SEN0644_BAUD);
    uart_flush_input(s->uart_num);
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "[%s] searching... (RX GPIO %d, TX GPIO %d)",
             s->name, s->rx_pin, s->tx_pin);
    uint32_t found = detect_baud(s);
    if (found == 0) {
        ESP_LOGW(TAG, "[%s] not found at any baud (check A/B, common GND, pins) -> offline",
                 s->name);
        s->online = false;
        return;
    }
    s->baud = found;
    s->online = true;
    ESP_LOGI(TAG, "[%s] found at %u baud", s->name, (unsigned)found);

    if (write_and_confirm(s, SET_LEVEL_1)) {
        ESP_LOGI(TAG, "[%s] acquisition level 1 confirmed", s->name);
    } else {
        ESP_LOGW(TAG, "[%s] level write not confirmed, continuing", s->name);
    }

    /* Rewrite the baud register on EVERY init so a sensor swapped in from
     * another rig always lands in a known state. */
    bool baud_written = false;
    for (uint8_t attempt = 1; attempt <= SEN0644_BAUD_WRITE_RETRIES && !baud_written; attempt++) {
        baud_written = write_and_confirm(s, SET_BAUD_9600);
        if (!baud_written) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!baud_written) {
        ESP_LOGW(TAG, "[%s] baud write not confirmed, staying at detected baud", s->name);
        uart_set_baudrate(s->uart_num, found);
        return;
    }
    if (found == SEN0644_BAUD) {
        ESP_LOGI(TAG, "[%s] baud already 9600, no power-cycle needed", s->name);
        return;
    }

    ESP_LOGW(TAG, "[%s] baud set to 9600 >>> POWER-CYCLE THIS SENSOR NOW (unplug ~3 s) <<<",
             s->name);
    uart_set_baudrate(s->uart_num, SEN0644_BAUD);
    int64_t wait_start = esp_timer_get_time();
    while (!sensor_responds(s)) {
        if (esp_timer_get_time() - wait_start > (int64_t)SEN0644_POWERCYCLE_WAIT_MS * 1000) {
            ESP_LOGW(TAG, "[%s] never came back at 9600 -> offline", s->name);
            s->online = false;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    s->baud = SEN0644_BAUD;
    ESP_LOGI(TAG, "[%s] back online at 9600", s->name);
}

static void publish_snapshot(const float *lux, const bool *ok)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_latest.timestamp_us = esp_timer_get_time();
    for (int i = 0; i < SEN0644_NUM_SENSORS; i++) {
        s_latest.online[i] = s_sensors[i].online;
        s_latest.valid[i]  = ok[i];
        if (ok[i]) s_latest.lux[i] = lux[i];
    }
    xSemaphoreGive(s_lock);
}

static void sen0644_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));   /* let the sensors boot */

    /* Sequential so any two power-cycle prompts never overlap. */
    for (int i = 0; i < SEN0644_NUM_SENSORS; i++) init_sensor(&s_sensors[i]);

    for (;;) {
        float lux[SEN0644_NUM_SENSORS];
        bool  ok[SEN0644_NUM_SENSORS];
        uint32_t raw;

        for (int i = 0; i < SEN0644_NUM_SENSORS; i++) {
            ok[i] = s_sensors[i].online && read_lux_raw(&s_sensors[i], &raw);
            if (ok[i]) lux[i] = raw / 1000.0f;
        }
        publish_snapshot(lux, ok);

        /* Recovery: online sensor that stops answering gets a full re-init after
         * RECOVER_AFTER_FAILS; an offline sensor is retried on a timer. */
        for (int i = 0; i < SEN0644_NUM_SENSORS; i++) {
            sensor_t *s = &s_sensors[i];
            if (s->online) {
                if (ok[i]) { s->consec_fail = 0; continue; }
                if (++s->consec_fail >= SEN0644_RECOVER_AFTER_FAILS) {
                    ESP_LOGW(TAG, "[%s] lost contact, re-running detection", s->name);
                    init_sensor(s);
                }
            } else if (esp_timer_get_time() - s->last_attempt_us
                       >= (int64_t)SEN0644_OFFLINE_RETRY_MS * 1000) {
                init_sensor(s);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SEN0644_POLL_PERIOD_MS));
    }
}

esp_err_t sen0644_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    const uart_config_t cfg = {
        .baud_rate  = SEN0644_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    for (int i = 0; i < SEN0644_NUM_SENSORS; i++) {
        sensor_t *s = &s_sensors[i];
        esp_err_t err = uart_driver_install(s->uart_num, UART_RX_BUF, 0, 0, NULL, 0);
        if (err != ESP_OK) return err;
        ESP_ERROR_CHECK(uart_param_config(s->uart_num, &cfg));
        ESP_ERROR_CHECK(uart_set_pin(s->uart_num, s->tx_pin, s->rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    }

    if (xTaskCreatePinnedToCore(sen0644_task, "sen0644", 4096, NULL,
                                PRIO_SEN0644, NULL, CORE_NETWORK) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sen0644_get_reading(sen0644_reading_t *out)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_latest;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
