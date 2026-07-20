/*
 * pac1951.c — talks to the PAC1951 voltage monitor over I2C. (See pac1951.h.)
 *
 * ============================== WHY WE NEED IT ==============================
 * The old AMC1311 was an analog part: it output a voltage the ESP32 measured
 * with its own ADC. The PAC1951 is smarter — it has its OWN 16-bit ADC and
 * digitizes the bus voltage itself, then hands us the number over I2C. So the
 * ESP32 no longer "measures" the voltage; it just READS A REGISTER.
 *
 * The PAC sits on the 600 V high-voltage side. The I2C wires reach it through an
 * ISO1540 isolator (a chip that copies SDA/SCL across the HV barrier so the
 * low-voltage ESP32 stays safe). The isolator is invisible to this code — from
 * the firmware's view the PAC is just another device on the same I2C bus as the
 * DS3231 clock.
 *
 * READING THE VOLTAGE: the PAC only updates its readable VBUS register when we
 * send a "REFRESH" command; otherwise it keeps showing the last value. We use
 * REFRESH_V (refresh the reading, but don't reset the chip's energy counters).
 * The sampler reads the last value and issues a new REFRESH each tick, so we
 * never sit and wait for a fresh conversion inside the 200 Hz loop.
 *
 * REGISTER BYTE ORDER: the PAC sends multi-byte registers most-significant byte
 * first (big-endian), so VBUS comes back as [high, low] and we recombine it as
 * (high << 8) | low.
 * ===========================================================================
 */
#include "pac1951.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"           /* vTaskDelay for the cal averaging pause */
#include "esp_log.h"

static const char *TAG = "pac1951";

#define I2C_TIMEOUT_MS     1000      /* give up an I2C read/write after 1 sec  */

/* The "ticket" for the PAC1951 as a device on the shared I2C bus. */
static i2c_master_dev_handle_t s_dev = NULL;

/* Send one of the PAC's bare command bytes (REFRESH / REFRESH_V). These are
 * "Send Byte" transactions: just the command, no data payload. */
static esp_err_t pac_command(uint8_t cmd)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(s_dev, &cmd, 1, I2C_TIMEOUT_MS);
}

/* Read a 16-bit register: send the register pointer, then read the two bytes
 * back in one combined transaction, and recombine them MSB-first. */
static esp_err_t pac_read_u16(uint8_t reg, uint16_t *out)
{
    if (s_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[2];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof(b), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    *out = (uint16_t)((b[0] << 8) | b[1]);  /* big-endian: [high, low] */
    return ESP_OK;
}

/* Register the PAC1951 on the shared I2C bus (created in main.c) and prime it. */
esp_err_t pac1951_init(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,  /* PAC uses a 7-bit address     */
        .device_address  = PAC1951_ADDR,        /* 0x10 (from config.h)         */
        .scl_speed_hz    = I2C_FREQ_HZ,         /* how fast to clock the bus    */
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Leave the CTRL register at its power-on default (1024 sps, VBUS unipolar
     * 0..32 V) — that suits a bench voltage read. Just kick one REFRESH so the
     * first VBUS read returns a real conversion instead of zero. */
    err = pac_command(PAC1951_REG_REFRESH);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "initial refresh failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PAC1951 ready at 0x%02X (VBUS over I2C)", PAC1951_ADDR);
    }
    return ESP_OK;  /* device added; a failed first refresh is non-fatal */
}

/* Refresh the readable registers without resetting the energy accumulators. */
esp_err_t pac1951_refresh_v(void)
{
    return pac_command(PAC1951_REG_REFRESH_V);
}

/* Most-recent VBUS conversion, raw 16-bit count. */
esp_err_t pac1951_read_vbus_raw(uint16_t *raw)
{
    return pac_read_u16(PAC1951_REG_VBUS, raw);
}

/* On-chip 8-sample rolling average of VBUS. We refresh, give the chip a moment,
 * then read the averaged register — lower noise for calibration. */
esp_err_t pac1951_read_vbus_avg_raw(uint16_t *raw)
{
    esp_err_t err = pac1951_refresh_v();
    if (err != ESP_OK) {
        return err;
    }
    /* The datasheet asks for ~1 ms after REFRESH before the results are stable;
     * this path runs only during the `cal v` command, so a short wait is fine. */
    vTaskDelay(pdMS_TO_TICKS(2));
    return pac_read_u16(PAC1951_REG_VBUS_AVG, raw);
}
