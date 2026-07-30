// SEN0644 RS485 ambient light sensor - ESP-IDF port of the Arduino sketch.
// Loops forever: read the 32-bit lux register over Modbus-RTU and print it.
// View live values over USB with:  pio device monitor  (115200 baud)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "config.h"
#include "modbus_master.h"

static const char *TAG = "sen0644";
static modbus_master_t sen0644;

static void uart_setup(void)
{
    uart_config_t cfg = {
        .baud_rate  = SEN_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,   // 8N1
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(SEN_UART_NUM, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(SEN_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(SEN_UART_NUM, SEN_TX_PIN, SEN_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void)
{
    uart_setup();
    mbm_begin(&sen0644, SEN_UART_NUM, SEN_SLAVE_ADDR, SEN_READ_TIMEOUT_MS);
    ESP_LOGI(TAG, "SEN0644 RS485 test started");

    while (1) {
        uint8_t result = mbm_read_holding_registers(&sen0644, SEN_LUX_REG, SEN_LUX_QTY);

        if (result == MBM_SUCCESS) {
            uint16_t high_word = mbm_get_response(&sen0644, 0);
            uint16_t low_word  = mbm_get_response(&sen0644, 1);
            uint32_t raw_lux   = ((uint32_t)high_word << 16) | low_word;
            float    lux       = raw_lux / SEN_LUX_DIVISOR;

            ESP_LOGI(TAG, "Lux: %.2f", lux);
        } else {
            ESP_LOGW(TAG, "Read failed. Error code: 0x%02X", result);
        }

        vTaskDelay(pdMS_TO_TICKS(SEN_POLL_MS));
    }
}
