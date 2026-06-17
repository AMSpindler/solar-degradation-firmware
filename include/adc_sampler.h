/*
 * adc_sampler.h — Core-0 real-time ADC sampling of the AMC1311 (voltage,
 * differential) and ACS724 (current) channels.
 *
 * An esp_timer fires periodically at the configured sample rate; its callback
 * reads ADC1, builds a SamplePacket, and pushes it to g_sample_queue. Raw ADC
 * counts are stored in the packet; engineering units are produced on demand by
 * apply_cal() using two-point slope/offset coefficients persisted in NVS.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "sample_packet.h"

/* calc = raw * slope + offset.  For voltage, "raw" is (VOUTP - VOUTN). */
typedef struct {
    float slope;
    float offset;
} cal_coeff_t;

/* Calibration channel indices. */
enum {
    CAL_CH_VOLTAGE = 0,
    CAL_CH_CURRENT = 1,
    CAL_CH_COUNT   = 2,
};

/* Lifecycle. init() creates the queue, ADC unit, eFuse cali, loads cal from
 * NVS and creates the (stopped) periodic timer. start()/stop() control sampling. */
esp_err_t adc_sampler_init(uint16_t sample_rate_hz);
esp_err_t adc_sampler_start(void);
esp_err_t adc_sampler_stop(void);
bool      adc_sampler_is_running(void);
uint16_t  adc_sampler_get_rate_hz(void);

/* One full packet, read synchronously (used by the `sample once` command). */
esp_err_t adc_sampler_read_once(SamplePacket *out);

/* Averaged raw readings for two-point calibration (`cal` command). Voltage
 * returns the averaged differential (VOUTP - VOUTN). */
esp_err_t adc_sampler_average_voltage_raw(int n, float *out_diff);
esp_err_t adc_sampler_average_current_raw(int n, float *out_raw);

/* Calibration access. set/reset update the RAM copy and persist to NVS. */
esp_err_t adc_sampler_get_cal(int ch, cal_coeff_t *out);
esp_err_t adc_sampler_set_cal(int ch, cal_coeff_t coeff);
esp_err_t adc_sampler_reset_cal(void);

/* Apply calibration to a raw packet -> engineering units (V, A). */
void adc_sampler_apply_cal(const SamplePacket *p, float *v_calc, float *i_calc);

/* Diagnostics: convert a raw ADC count to millivolts via eFuse calibration.
 * Returns false if the curve-fitting scheme is unavailable. */
bool adc_sampler_raw_to_mv(uint16_t raw, int *out_mv);
