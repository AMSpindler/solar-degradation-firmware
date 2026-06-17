/*
 * adc_sampler.c — see adc_sampler.h.
 *
 * Hardware/pin mapping lives in config.h. All sampling is on ADC1 (ADC2 is
 * unusable while WiFi is active). The periodic esp_timer callback runs in the
 * esp_timer service task (pinned to CPU0 via CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0),
 * not a hardware ISR, so blocking-ish adc_oneshot_read() + xQueueSend() are safe.
 */
#include "adc_sampler.h"
#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "adc_sampler";

QueueHandle_t g_sample_queue = NULL;

static adc_oneshot_unit_handle_t s_adc          = NULL;
static adc_cali_handle_t         s_cali         = NULL;  /* NULL if unsupported */
static esp_timer_handle_t        s_timer        = NULL;
static uint16_t                  s_rate_hz      = SAMPLE_RATE_HZ_DEFAULT;
static volatile bool             s_running      = false;
static cal_coeff_t               s_cal[CAL_CH_COUNT];

/* ------------------------------------------------------------------------- */
/* Low-level reads                                                           */
/* ------------------------------------------------------------------------- */

static inline uint16_t read_raw(adc_channel_t ch)
{
    int raw = 0;
    /* On error, return 0; the consumer will see a flat-line which the operator
     * notices immediately on the plotter. Avoids logging from the timer task. */
    if (adc_oneshot_read(s_adc, ch, &raw) != ESP_OK) {
        return 0;
    }
    return (uint16_t)raw;
}

static void build_packet(SamplePacket *p)
{
    p->timestamp_us    = (uint64_t)esp_timer_get_time();
    p->voltage_raw     = read_raw(ADC_VOLTAGE_P_CHANNEL);
    p->aux_channels[0] = read_raw(ADC_VOLTAGE_N_CHANNEL);
    p->current_raw     = read_raw(ADC_CURRENT_CHANNEL);
    p->aux_channels[1] = read_raw(ADC_AUX_CHANNEL);
}

static void sample_timer_cb(void *arg)
{
    (void)arg;
    SamplePacket pkt;
    build_packet(&pkt);
    /* Non-blocking: if the queue is full (consumer stalled) we drop the sample
     * rather than stall the sampling cadence. */
    (void)xQueueSend(g_sample_queue, &pkt, 0);
}

/* ------------------------------------------------------------------------- */
/* Calibration persistence                                                   */
/* ------------------------------------------------------------------------- */

static const char *cal_key(int ch)
{
    return (ch == CAL_CH_VOLTAGE) ? "v_cal" : "i_cal";
}

static void cal_load_from_nvs(void)
{
    /* Defaults: identity (calc == raw / raw-diff) until calibrated. */
    for (int i = 0; i < CAL_CH_COUNT; i++) {
        s_cal[i].slope = 1.0f;
        s_cal[i].offset = 0.0f;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_CAL_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no calibration in NVS; using identity coefficients");
        return;
    }
    for (int i = 0; i < CAL_CH_COUNT; i++) {
        cal_coeff_t c;
        size_t len = sizeof(c);
        if (nvs_get_blob(h, cal_key(i), &c, &len) == ESP_OK && len == sizeof(c)) {
            s_cal[i] = c;
            ESP_LOGI(TAG, "cal[%d] loaded: slope=%.6g offset=%.6g", i, c.slope, c.offset);
        }
    }
    nvs_close(h);
}

static esp_err_t cal_save_to_nvs(int ch)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_CAL_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, cal_key(ch), &s_cal[ch], sizeof(cal_coeff_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t adc_sampler_init(uint16_t sample_rate_hz)
{
    if (sample_rate_hz < SAMPLE_RATE_HZ_MIN || sample_rate_hz > SAMPLE_RATE_HZ_MAX) {
        ESP_LOGW(TAG, "rate %u out of range; clamping", sample_rate_hz);
        if (sample_rate_hz < SAMPLE_RATE_HZ_MIN) sample_rate_hz = SAMPLE_RATE_HZ_MIN;
        if (sample_rate_hz > SAMPLE_RATE_HZ_MAX) sample_rate_hz = SAMPLE_RATE_HZ_MAX;
    }
    s_rate_hz = sample_rate_hz;

    g_sample_queue = xQueueCreate(SAMPLE_QUEUE_LEN, sizeof(SamplePacket));
    if (g_sample_queue == NULL) {
        ESP_LOGE(TAG, "queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* ADC1 oneshot unit + channels. */
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN, .bitwidth = ADC_BITWIDTH };
    const adc_channel_t channels[] = {
        ADC_VOLTAGE_P_CHANNEL, ADC_VOLTAGE_N_CHANNEL,
        ADC_CURRENT_CHANNEL,   ADC_AUX_CHANNEL,
    };
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, channels[i], &chan_cfg));
    }

    /* eFuse curve-fitting calibration (diagnostics: raw -> mV). */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT,
        .atten    = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        ESP_LOGI(TAG, "eFuse curve-fitting calibration enabled");
    } else {
        ESP_LOGW(TAG, "eFuse calibration unavailable; raw->mV disabled");
        s_cali = NULL;
    }
#else
    ESP_LOGW(TAG, "curve-fitting scheme not supported on this target");
#endif

    cal_load_from_nvs();

    /* Periodic timer (created stopped). Dispatched from the esp_timer task. */
    const esp_timer_create_args_t targs = {
        .callback        = sample_timer_cb,
        .name            = "adc_smp",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_timer));

    ESP_LOGI(TAG, "init done: %u Hz, queue depth %d", s_rate_hz, SAMPLE_QUEUE_LEN);
    return ESP_OK;
}

esp_err_t adc_sampler_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    uint64_t period_us = 1000000ULL / s_rate_hz;
    esp_err_t err = esp_timer_start_periodic(s_timer, period_us);
    if (err == ESP_OK) {
        s_running = true;
        ESP_LOGI(TAG, "sampling started (%llu us period)", period_us);
    }
    return err;
}

esp_err_t adc_sampler_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }
    esp_err_t err = esp_timer_stop(s_timer);
    if (err == ESP_OK) {
        s_running = false;
        ESP_LOGI(TAG, "sampling stopped");
    }
    return err;
}

bool     adc_sampler_is_running(void) { return s_running; }
uint16_t adc_sampler_get_rate_hz(void) { return s_rate_hz; }

esp_err_t adc_sampler_read_once(SamplePacket *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    build_packet(out);
    return ESP_OK;
}

esp_err_t adc_sampler_average_voltage_raw(int n, float *out_diff)
{
    if (out_diff == NULL || n <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        int p = read_raw(ADC_VOLTAGE_P_CHANNEL);
        int q = read_raw(ADC_VOLTAGE_N_CHANNEL);
        acc += (double)(p - q);
    }
    *out_diff = (float)(acc / n);
    return ESP_OK;
}

esp_err_t adc_sampler_average_current_raw(int n, float *out_raw)
{
    if (out_raw == NULL || n <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        acc += (double)read_raw(ADC_CURRENT_CHANNEL);
    }
    *out_raw = (float)(acc / n);
    return ESP_OK;
}

esp_err_t adc_sampler_get_cal(int ch, cal_coeff_t *out)
{
    if (ch < 0 || ch >= CAL_CH_COUNT || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_cal[ch];
    return ESP_OK;
}

esp_err_t adc_sampler_set_cal(int ch, cal_coeff_t coeff)
{
    if (ch < 0 || ch >= CAL_CH_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cal[ch] = coeff;
    return cal_save_to_nvs(ch);
}

esp_err_t adc_sampler_reset_cal(void)
{
    esp_err_t err = ESP_OK;
    for (int i = 0; i < CAL_CH_COUNT; i++) {
        s_cal[i].slope = 1.0f;
        s_cal[i].offset = 0.0f;
        esp_err_t e = cal_save_to_nvs(i);
        if (e != ESP_OK) {
            err = e;
        }
    }
    return err;
}

void adc_sampler_apply_cal(const SamplePacket *p, float *v_calc, float *i_calc)
{
    int v_diff = (int)p->voltage_raw - (int)p->aux_channels[0];
    if (v_calc) {
        *v_calc = s_cal[CAL_CH_VOLTAGE].slope * (float)v_diff
                  + s_cal[CAL_CH_VOLTAGE].offset;
    }
    if (i_calc) {
        *i_calc = s_cal[CAL_CH_CURRENT].slope * (float)p->current_raw
                  + s_cal[CAL_CH_CURRENT].offset;
    }
}

bool adc_sampler_raw_to_mv(uint16_t raw, int *out_mv)
{
    if (s_cali == NULL || out_mv == NULL) {
        return false;
    }
    return adc_cali_raw_to_voltage(s_cali, raw, out_mv) == ESP_OK;
}
