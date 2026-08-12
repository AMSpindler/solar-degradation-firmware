/*
 * adc_sampler.c — reads the voltage and current sensors. (See adc_sampler.h.)
 *
 * ============================== WHAT IT DOES ==============================
 * The ESP32 has a built-in ADC (Analog-to-Digital Converter). An ADC turns a
 * real-world voltage (like 1.37 volts) into a number the code can use. This
 * chip's ADC gives a number from 0 to 4095 (that's "12-bit"), where 0 means
 * ~0 V and 4095 means the top of the range (~3.1 V here).
 *
 * Each sample now comes from TWO different places:
 *   - VOLTAGE: read as a digital number from the PAC1951 monitor over I2C (it
 *     has its own ADC). This replaced the old AMC1311 analog path — the ESP32
 *     ADC no longer measures voltage at all.
 *   - CURRENT: still the HSTS016L, a differential analog sensor (Vout and Vref)
 *     read on two ESP32 ADC channels; the signal is the difference Vout - Vref.
 *     "Differential" means the real reading is the gap between the two pins,
 *     which cancels offset/supply drift shared by both.
 *
 * A hardware timer fires 200 times per second. Each time, our callback reads the
 * PAC1951 (I2C) and the current ADC channels, packs the numbers into a
 * SamplePacket, and drops it on a "queue" (a thread-safe conveyor belt).
 *
 * IMPORTANT: we only use ADC1. The ESP32's other ADC ("ADC2") shares circuitry
 * with Wi-Fi and stops working once Wi-Fi turns on, so we avoid it.
 * =========================================================================
 */
#include "adc_sampler.h"
#include "config.h"
#include "pac1951.h"                 /* voltage now comes from the PAC1951    */

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"               /* high-precision repeating timer        */
#include "esp_adc/adc_oneshot.h"     /* read the ADC one value at a time      */
#include "esp_adc/adc_cali.h"        /* factory calibration: raw -> millivolts*/
#include "esp_adc/adc_cali_scheme.h"
#include "nvs.h"                     /* save/load calibration permanently     */
#include "nvs_flash.h"

static const char *TAG = "adc_sampler";

/*
 * The shared "conveyor belt". `extern` in the header means other files can see
 * it; here is where it actually lives. It starts empty (NULL) until init()
 * creates it. xQueueSend puts a SamplePacket on; xQueueReceive takes one off.
 */
QueueHandle_t g_sample_queue = NULL;

/* "static" here means these are private to this file — nothing outside can
 * touch them. They hold the state of the sampler between function calls. */
static adc_oneshot_unit_handle_t s_adc     = NULL;  /* ticket for the ADC          */
static adc_cali_handle_t         s_cali    = NULL;  /* ticket for raw->mV (or NULL)*/
static esp_timer_handle_t        s_timer   = NULL;  /* ticket for the timer        */
static uint16_t                  s_rate_hz = SAMPLE_RATE_HZ_DEFAULT;
static volatile bool             s_running = false;  /* is the timer going?        */
/* The calibration numbers, one set per channel (voltage and current).             */
static cal_coeff_t               s_cal[CAL_CH_COUNT];

/* Fan-out: every reading is copied to each of these subscriber queues. The
 * console plot, the Wi-Fi sender, and the SD logger each register one. */
static QueueHandle_t             s_subs[MAX_SAMPLE_SUBSCRIBERS];
static int                       s_nsubs = 0;

/* Wire passes through the current sensor hole (physics amps conversion). */
static int                       s_turns = CURRENT_TURNS_DEFAULT;

/* Reads averaged per sample (1 = off). Set live with the `avg` command. */
static int                       s_oversample = ADC_OVERSAMPLE_DEFAULT;

/* ------------------------------------------------------------------------- */
/* Low-level reads                                                           */
/* ------------------------------------------------------------------------- */

/* Read one raw number (0..4095) from a single ADC channel. "inline" is a hint
 * to the compiler that this is tiny — don't worry about it. */
static inline uint16_t read_raw(adc_channel_t ch)
{
    /* Average s_oversample reads to cut noise (s_oversample=1 => a single read).
     * Failed reads are skipped. On the plotter, a stuck 0 line is an obvious
     * "something's wrong" signal. We deliberately do NOT print errors here:
     * this runs 200x/second, and printing that often would jam everything. */
    int raw, acc = 0, got = 0;
    for (int i = 0; i < s_oversample; i++) {
        if (adc_oneshot_read(s_adc, ch, &raw) == ESP_OK) {
            acc += raw;
            got++;
        }
    }
    return got ? (uint16_t)(acc / got) : 0;
}

/* Fill in one complete SamplePacket: a timestamp, the PAC1951 voltage (I2C),
 * and the two current ADC legs. */
static void build_packet(SamplePacket *p)
{
    /* esp_timer_get_time() = microseconds since the chip booted.
     * Good for measuring exact spacing between samples. */
    p->timestamp_us    = (uint64_t)esp_timer_get_time();

    /* VOLTAGE over I2C: read the value the PAC refreshed on the PREVIOUS tick,
     * then ask it to refresh for the NEXT tick. This "pipelining" keeps the loop
     * from blocking on a conversion. If the read fails, leave 0 (an obvious
     * "something's wrong" flatline, same as a stuck ADC). aux[0] is unused now
     * (the old VOUTN leg is gone) — keep it 0 so the differential in apply_cal
     * reduces to just the VBUS value. */
#if ENABLE_PAC1951
    uint16_t vbus = 0;
    (void)pac1951_read_vbus_raw(&vbus);
    p->voltage_raw     = vbus;
    p->aux_channels[0] = 0;
    (void)pac1951_refresh_v();  /* prime the next read */
#else
    /* PAC not wired yet — report 0 volts and never touch the I2C bus, so a
     * missing chip can't stall the sampler with I2C timeouts. */
    p->voltage_raw     = 0;
    p->aux_channels[0] = 0;
#endif

    /* CURRENT still on the ESP32 ADC (unchanged). */
    p->current_raw     = read_raw(ADC_CURRENT_CHANNEL);     /* HSTS016L Vout */
    p->aux_channels[1] = read_raw(ADC_CURRENT_REF_CHANNEL); /* HSTS016L Vref */
}

/*
 * This is the "callback": the function the timer automatically runs 200x/sec.
 * It must be quick and must not get stuck waiting on anything, or it will throw
 * off the sampling rhythm. So all it does is read + drop the reading on the queue.
 */
static void sample_timer_cb(void *arg)
{
    (void)arg;  /* we don't use this argument; this line silences a warning   */
    SamplePacket pkt;
    build_packet(&pkt);

    /* Copy the reading to every subscriber's queue. The last argument "0" means
     * "don't wait": if a queue is full (that consumer fell behind), drop the
     * sample for that one consumer rather than stall the whole sampler. Each
     * consumer's queue is independent, so a slow SD card can't starve Wi-Fi. */
    for (int i = 0; i < s_nsubs; i++) {
        (void)xQueueSend(s_subs[i], &pkt, 0);
    }
}

/* ------------------------------------------------------------------------- */
/* Calibration persistence (saving/loading to NVS)                           */
/* ------------------------------------------------------------------------- */
/*
 * Calibration turns a raw ADC number into a real-world value. 
 * Use a simple straight line equation: value = raw * slope + offset
 * The `cal` console command figures out slope & offset by measuring two known
 * points. We save them in NVS so they survive a power-off.
 */

/* The little text "key" each channel's numbers are filed under in NVS. */
static const char *cal_key(int ch)
{
    return (ch == CAL_CH_VOLTAGE) ? "v_cal" : "i_cal";
}

/* Load saved calibration at boot. If nothing is saved yet, fall back to an
 * "identity" line (slope=1, offset=0) so value == raw until you calibrate. */
static void cal_load_from_nvs(void)
{
    for (int i = 0; i < CAL_CH_COUNT; i++) {
        s_cal[i].slope = 1.0f;
        s_cal[i].offset = 0.0f;
    }

    nvs_handle_t h;
    /* Open our storage "drawer" named "cal" for reading. */
    if (nvs_open(NVS_CAL_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no calibration in NVS; using identity coefficients");
        return;
    }
    for (int i = 0; i < CAL_CH_COUNT; i++) {
        cal_coeff_t c;
        size_t len = sizeof(c);
        /* A "blob" is just a chunk of raw bytes. We saved the slope + offset as
         * one blob; here we read it back. */
        if (nvs_get_blob(h, cal_key(i), &c, &len) == ESP_OK && len == sizeof(c)) {
            s_cal[i] = c;
            ESP_LOGI(TAG, "cal[%d] loaded: slope = %.6g | offset = %.6g", i, c.slope, c.offset);
        }
    }
    nvs_close(h);  /* always close the drawer when done */
}

/* Save one channel's calibration to NVS. */
static esp_err_t cal_save_to_nvs(int ch)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_CAL_NAMESPACE, NVS_READWRITE, &h);  /* open for writing */
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, cal_key(ch), &s_cal[ch], sizeof(cal_coeff_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);  /* commit = actually write it to flash */
    }
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------------- */
/* Public API (the functions other files are allowed to call)                */
/* ------------------------------------------------------------------------- */

/* Register a queue to receive a copy of every reading. */
esp_err_t adc_sampler_subscribe(QueueHandle_t q)
{
    if (q == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_nsubs >= MAX_SAMPLE_SUBSCRIBERS) {
        ESP_LOGE(TAG, "too many subscribers (max %d)", MAX_SAMPLE_SUBSCRIBERS);
        return ESP_ERR_NO_MEM;
    }
    s_subs[s_nsubs++] = q;
    return ESP_OK;
}

/* One-time setup. Called once from app_main() before sampling starts. */
esp_err_t adc_sampler_init(uint16_t sample_rate_hz)
{
    /* Keep the requested rate inside the sane 100..500 Hz range. */
    if (sample_rate_hz < SAMPLE_RATE_HZ_MIN || sample_rate_hz > SAMPLE_RATE_HZ_MAX) {
        ESP_LOGW(TAG, "rate %u out of range; clamping", sample_rate_hz);
        if (sample_rate_hz < SAMPLE_RATE_HZ_MIN) sample_rate_hz = SAMPLE_RATE_HZ_MIN;
        if (sample_rate_hz > SAMPLE_RATE_HZ_MAX) sample_rate_hz = SAMPLE_RATE_HZ_MAX;
    }
    s_rate_hz = sample_rate_hz;

    /* Create the console-plot conveyor belt and register it as subscriber #0,
     * so the `plot` command works exactly as before. Wi-Fi/SD add their own. */
    g_sample_queue = xQueueCreate(SAMPLE_QUEUE_LEN, sizeof(SamplePacket));
    if (g_sample_queue == NULL) {
        ESP_LOGE(TAG, "queue alloc failed");
        return ESP_ERR_NO_MEM;  /* "out of memory" */
    }
    adc_sampler_subscribe(g_sample_queue);

    /* Turn on ADC1. */
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    /* Configure each channel we read. "atten" (attenuation) sets the voltage
     * range; DB_12 gives the widest range (~0..3.1 V). "bitwidth" = 12 bits. */
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN, .bitwidth = ADC_BITWIDTH };
    /* Only the current sensor is on the ADC now; voltage comes from the PAC1951
     * over I2C (registered separately in main.c via pac1951_init). */
    const adc_channel_t channels[] = {
        ADC_CURRENT_CHANNEL, ADC_CURRENT_REF_CHANNEL,
    };
    /* Loop over the list and configure each one. sizeof(arr)/sizeof(arr[0])
     * is the classic C way to count items in an array. */
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, channels[i], &chan_cfg));
    }

    /* Every ESP32 is calibrated at the factory. This optional step lets us
     * convert a raw number to millivolts accurately — handy for the `sample`
     * command to sanity-check wiring. If unavailable, we just skip it. */
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

    cal_load_from_nvs();  /* load your saved slope/offset (or defaults) */

    /* Create the repeating timer, but don't start it yet. ESP_TIMER_TASK means
     * "run my callback as a normal task" (safe to do ADC reads inside it). */
    const esp_timer_create_args_t targs = {
        .callback        = sample_timer_cb,  /* the function to run each tick   */
        .name            = "adc_smp",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_timer));

    ESP_LOGI(TAG, "init done: %u Hz, queue depth %d", s_rate_hz, SAMPLE_QUEUE_LEN);
    return ESP_OK;
}

/* Start the timer -> sampling begins. */
esp_err_t adc_sampler_start(void)
{
    if (s_running) {
        return ESP_OK;  /* already running; nothing to do */
    }
    /* Period in microseconds = 1,000,000 / rate. For 200 Hz that's 5000 us. */
    uint64_t period_us = 1000000ULL / s_rate_hz;
    esp_err_t err = esp_timer_start_periodic(s_timer, period_us);
    if (err == ESP_OK) {
        s_running = true;
        ESP_LOGI(TAG, "sampling started (%llu us period)", period_us);
    }
    return err;
}

/* Stop the timer -> sampling pauses. */
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

/* Read one packet right now (used by the `sample once` command). */
esp_err_t adc_sampler_read_once(SamplePacket *out)
{
    if (out == NULL) {                 /* caller must give us somewhere to write */
        return ESP_ERR_INVALID_ARG;
    }
    build_packet(out);
    return ESP_OK;
}

/*
 * Average several voltage readings for calibration. The PAC1951 already keeps an
 * 8-sample rolling average of VBUS on-chip; we read that a few times and average
 * again to further steady the `cal v` math. Returns the averaged raw VBUS count
 * (there is no differential leg anymore — the PAC gives a single value).
 */
esp_err_t adc_sampler_average_voltage_raw(int n, float *out_diff)
{
    if (out_diff == NULL || n <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
#if ENABLE_PAC1951
    double acc = 0.0;  /* running total; double = high-precision decimal */
    int got = 0;
    for (int i = 0; i < n; i++) {
        uint16_t vbus = 0;
        if (pac1951_read_vbus_avg_raw(&vbus) == ESP_OK) {
            acc += (double)vbus;
            got++;
        }
    }
    *out_diff = got ? (float)(acc / got) : 0.0f;  /* total / count = average */
    return got ? ESP_OK : ESP_FAIL;
#else
    /* PAC disabled — no voltage source to calibrate. */
    *out_diff = 0.0f;
    return ESP_OK;
#endif
}

/* Same idea for the current sensor. Returns the averaged DIFFERENCE
 * (Vout - Vref), the HSTS016L's real current signal. */
esp_err_t adc_sampler_average_current_raw(int n, float *out_diff)
{
    if (out_diff == NULL || n <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        int p = read_raw(ADC_CURRENT_CHANNEL);      /* HSTS016L Vout */
        int q = read_raw(ADC_CURRENT_REF_CHANNEL);  /* HSTS016L Vref */
        acc += (double)(p - q);
    }
    *out_diff = (float)(acc / n);
    return ESP_OK;
}

/* Copy out the current calibration for one channel. */
esp_err_t adc_sampler_get_cal(int ch, cal_coeff_t *out)
{
    if (ch < 0 || ch >= CAL_CH_COUNT || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_cal[ch];
    return ESP_OK;
}

/* Set new calibration for one channel and save it to NVS. */
esp_err_t adc_sampler_set_cal(int ch, cal_coeff_t coeff)
{
    if (ch < 0 || ch >= CAL_CH_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cal[ch] = coeff;
    return cal_save_to_nvs(ch);
}

/* Forget calibration: back to slope=1, offset=0 (value == raw). */
esp_err_t adc_sampler_reset_cal(void)
{
    esp_err_t err = ESP_OK;
    for (int i = 0; i < CAL_CH_COUNT; i++) {
        s_cal[i].slope = 1.0f;
        s_cal[i].offset = 0.0f;
        esp_err_t e = cal_save_to_nvs(i);
        if (e != ESP_OK) {
            err = e;  /* remember the last error but keep trying the rest */
        }
    }
    return err;
}

void adc_sampler_set_turns(int turns) { s_turns = (turns < 1) ? 1 : turns; }
int  adc_sampler_get_turns(void) { return s_turns; }

void adc_sampler_set_oversample(int n) { s_oversample = (n < 1) ? 1 : n; }
int  adc_sampler_get_oversample(void) { return s_oversample; }

/* Raw count -> millivolts via factory calibration (falls back to a linear
 * estimate if unavailable). Internal float helper for the amps conversion. */
static float chan_mv(uint16_t raw)
{
    int mv;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return (float)mv;
    }
    return (float)raw * 3300.0f / 4095.0f;
}

/*
 * Turn a raw packet into real engineering units (volts and amps).
 * Both channels: if NOT two-point calibrated (slope=1, offset=0), use datasheet
 * physics so they read real units out of the box; once `cal v`/`cal i` runs, the
 * two-point line takes over.
 *   Voltage: physics = VBUS count / full-scale * FSR = volts at the PAC's SENSE1+
 *            pin (i.e. AFTER the external divider). `cal v` maps raw -> real bus
 *            volts and absorbs the divider ratio.
 *   Current: physics = (Vout - Vref, mV) / sensitivity / turns. `cal i` bakes the
 *            turn count into the slope.
 */
void adc_sampler_apply_cal(const SamplePacket *p, float *v_calc, float *i_calc)
{
    /* Voltage is a single PAC1951 count (aux[0] is 0, so v_diff is just
     * voltage_raw). Current is a differential: Vout - Vref. */
    int v_diff = (int)p->voltage_raw - (int)p->aux_channels[0];
    int i_diff = (int)p->current_raw - (int)p->aux_channels[1];
    if (v_calc) {
        cal_coeff_t cv = s_cal[CAL_CH_VOLTAGE];
        if (cv.slope == 1.0f && cv.offset == 0.0f) {
            /* Physics: convert the raw VBUS count to volts at the pin. */
            *v_calc = (float)p->voltage_raw / PAC1951_VBUS_FULL_SCALE * PAC1951_VBUS_FSR_V;
        } else {
            *v_calc = cv.slope * (float)v_diff + cv.offset;
        }
    }
    if (i_calc) {
        cal_coeff_t c = s_cal[CAL_CH_CURRENT];
        if (c.slope == 1.0f && c.offset == 0.0f) {
            float mv = chan_mv(p->current_raw) - chan_mv(p->aux_channels[1]);
            *i_calc = mv / CURRENT_SENS_MV_PER_A / (float)s_turns;
        } else {
            *i_calc = c.slope * (float)i_diff + c.offset;
        }
    }
}

/* Convert a raw number to millivolts using the factory calibration. Returns
 * false if that calibration wasn't available. Used only for diagnostics. */
bool adc_sampler_raw_to_mv(uint16_t raw, int *out_mv)
{
    if (s_cali == NULL || out_mv == NULL) {
        return false;
    }
    return adc_cali_raw_to_voltage(s_cali, raw, out_mv) == ESP_OK;
}
