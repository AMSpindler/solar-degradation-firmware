/*
 * adc_sampler.h — the "menu" of functions the ADC sampler offers.
 *
 * Other files #include this to learn what adc_sampler.c can do. The actual code
 * is in adc_sampler.c. In short: it reads voltage from the PAC1951 (over I2C) and
 * current from the HSTS016L (differential, on the ADC) on a timer, drops readings
 * on g_sample_queue, and converts raw numbers to real units using saved calibration.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"          /* esp_err_t = a return code: ESP_OK or an error */
#include "sample_packet.h"

/*
 * One channel's calibration: the straight line  value = raw * slope + offset.
 * For voltage, "raw" means the difference (VOUTP - VOUTN).
 */
typedef struct {
    float slope;
    float offset;
} cal_coeff_t;

/* We calibrate two channels. These names are nicer than bare 0/1 in the code. */
enum {
    CAL_CH_VOLTAGE = 0,
    CAL_CH_CURRENT = 1,
    CAL_CH_COUNT   = 2,
};

/* ---- Lifecycle: set up, then start/stop the sampling timer ---- */
esp_err_t adc_sampler_init(uint16_t sample_rate_hz);  /* call once at boot     */
esp_err_t adc_sampler_start(void);                    /* begin sampling        */
esp_err_t adc_sampler_stop(void);                     /* pause sampling        */
bool      adc_sampler_is_running(void);
uint16_t  adc_sampler_get_rate_hz(void);

/* Subscribe a queue to the sample stream. Every reading is copied (non-blocking)
 * to each subscribed queue, so consumers (plot, Wi-Fi, SD) never steal data from
 * one another. Call this before adc_sampler_start(). The caller creates and owns
 * the queue (element size must be sizeof(SamplePacket)). */
esp_err_t adc_sampler_subscribe(QueueHandle_t q);

/* Read one packet right now (used by the `sample once` command). */
esp_err_t adc_sampler_read_once(SamplePacket *out);

/* Average many readings — steadier numbers for calibration (`cal` command).
 * Both return the averaged differential: voltage (VOUTP - VOUTN), current
 * (Vout - Vref). */
esp_err_t adc_sampler_average_voltage_raw(int n, float *out_diff);
esp_err_t adc_sampler_average_current_raw(int n, float *out_diff);

/* ---- Calibration: read, set (saves to NVS), or reset to defaults ---- */
esp_err_t adc_sampler_get_cal(int ch, cal_coeff_t *out);
esp_err_t adc_sampler_set_cal(int ch, cal_coeff_t coeff);
esp_err_t adc_sampler_reset_cal(void);

/* Number of times the wire passes through the current sensor's hole. Used by
 * the physics amps conversion (divides the effective current back out). Only
 * matters when current is NOT two-point calibrated. */
void adc_sampler_set_turns(int turns);
int  adc_sampler_get_turns(void);

/* Reads averaged per sample to cut noise (1 = off). Higher = cleaner but more
 * ADC work; at 200 Hz keep <= ~16 or the sampler can't finish each 5 ms window. */
void adc_sampler_set_oversample(int n);
int  adc_sampler_get_oversample(void);

/* Turn a raw packet into real volts and amps. v_calc/i_calc are where the
 * answers get written (pass NULL for one you don't need). */
void adc_sampler_apply_cal(const SamplePacket *p, float *v_calc, float *i_calc);

/* Diagnostics only: raw ADC count -> millivolts via the factory calibration.
 * Returns false if that calibration isn't available on this chip. */
bool adc_sampler_raw_to_mv(uint16_t raw, int *out_mv);
