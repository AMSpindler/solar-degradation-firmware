/*
 * main.c — HSTS016L current-sensor tester (ESP32-S3, ESP-IDF).
 *
 * A small, standalone program to prove out the HSTS016L Hall-effect current
 * sensor: read its two analog outputs, convert to a real current in amps, and
 * print the values over USB — live as CSV or one reading at a time. No Wi-Fi,
 * no SD card, no voltage sensor. Just the current sensor.
 *
 * HOW THE SENSOR WORKS
 *   The HSTS016L is powered at 5 V and outputs a voltage centered on a 2.5 V
 *   reference (its "Vref" pin). The current signal is the DIFFERENCE between its
 *   output (Vout) and that reference (Vref):
 *       (Vout - Vref) = sensitivity * current
 *   For the 20 A model, sensitivity = 0.625 V / 20 A = 31.25 mV per amp.
 *   Reading Vout - Vref (instead of Vout alone) cancels supply/offset drift.
 *
 * WIRING (matches the current bench setup)
 *   Red    V+   -> 5V pin
 *   Black  0V   -> GND
 *   Yellow Vout -> GPIO2   (ADC1 channel 1)
 *   White  Vref -> GPIO1   (ADC1 channel 0)
 *   The current-carrying wire passes THROUGH the sensor's hole (see "turns").
 *
 * CONSOLE COMMANDS (type into the USB serial monitor)
 *   read           one reading: raw counts, millivolts, and amps
 *   stream [off]   continuous CSV: t_ms,iout_raw,iref_raw,mv_diff,amps
 *   zero           capture the zero-current offset (run with NO current)
 *   turns <n>      how many times the wire passes through the hole (see below)
 *   status         show the current configuration
 *
 * THE "TURNS" TRICK
 *   A clamp Hall sensor reads ampere-turns. A 20 A sensor barely resolves small
 *   bench currents (~100 mA is only a few mV). Loop the wire through the hole N
 *   times and it sees N x current; set `turns N` and the amps read out correctly.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_console.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/* ---- Configuration (change here if your wiring/sensor differs) ---- */
#define ADC_UNIT_ID       ADC_UNIT_1
#define CH_VOUT           ADC_CHANNEL_5   /* GPIO6 — HSTS016L Vout (yellow) */
#define CH_VREF           ADC_CHANNEL_4   /* GPIO5 — HSTS016L Vref (white)  */
#define ADC_ATTEN_CFG     ADC_ATTEN_DB_12 /* full-scale ~0..3.1 V           */
#define ADC_BITWIDTH_CFG  ADC_BITWIDTH_12 /* 0..4095                        */
#define SENS_MV_PER_A     31.25f          /* 20 A HSTS016L: 0.625 V / 20 A  */
#define AVG_N             64              /* reads averaged per measurement */
#define STREAM_HZ         10              /* CSV rows per second            */

static adc_oneshot_unit_handle_t s_adc  = NULL;
static adc_cali_handle_t         s_cali = NULL;   /* raw->mV; NULL if unavailable */
static float                     s_zero_mv = 0.0f; /* zero-current offset  */
static int                       s_turns   = 1;    /* wire passes through hole */

static TaskHandle_t  s_stream_task = NULL;
static volatile bool s_stream_run  = false;

/* ---- Low-level: averaged read + raw->millivolts ---- */

static int read_avg_raw(adc_channel_t ch, int n)
{
    long acc = 0;
    int raw;
    for (int i = 0; i < n; i++) {
        if (adc_oneshot_read(s_adc, ch, &raw) == ESP_OK) {
            acc += raw;
        }
    }
    return (int)(acc / n);
}

static float raw_to_mv(int raw)
{
    int mv;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return (float)mv;                       /* factory-calibrated mV     */
    }
    return raw * 3300.0f / 4095.0f;             /* fallback linear estimate  */
}

/* Take one measurement: raw counts, the Vout-Vref difference in mV, and amps. */
static void measure(int *iout_raw, int *iref_raw, float *mv_diff, float *amps)
{
    int vo = read_avg_raw(CH_VOUT, AVG_N);
    int vr = read_avg_raw(CH_VREF, AVG_N);
    float mvd = raw_to_mv(vo) - raw_to_mv(vr);
    *iout_raw = vo;
    *iref_raw = vr;
    *mv_diff  = mvd;
    /* Subtract the zero offset, divide by sensitivity, undo the turns count. */
    *amps = (mvd - s_zero_mv) / SENS_MV_PER_A / (float)s_turns;
}

/* ---- Console commands ---- */

static int cmd_read(int argc, char **argv)
{
    (void)argc; (void)argv;
    int vo, vr; float mvd, a;
    measure(&vo, &vr, &mvd, &a);
    printf("IOUT=%d (%.1f mV)  IREF=%d (%.1f mV)  diff=%.2f mV  I=%.4f A  [turns=%d zero=%.2f mV]\n",
           vo, raw_to_mv(vo), vr, raw_to_mv(vr), mvd, a, s_turns, s_zero_mv);
    return 0;
}

static void stream_task(void *arg)
{
    (void)arg;
    printf("t_ms,iout_raw,iref_raw,mv_diff,amps\n");   /* CSV header */
    while (s_stream_run) {
        int vo, vr; float mvd, a;
        measure(&vo, &vr, &mvd, &a);
        printf("%llu,%d,%d,%.3f,%.4f\n",
               (unsigned long long)(esp_timer_get_time() / 1000), vo, vr, mvd, a);
        vTaskDelay(pdMS_TO_TICKS(1000 / STREAM_HZ));
    }
    s_stream_task = NULL;
    vTaskDelete(NULL);
}

static int cmd_stream(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        s_stream_run = false;
        printf("stream off\n");
        return 0;
    }
    if (s_stream_task != NULL) {
        printf("already streaming; type `stream off` to stop\n");
        return 0;
    }
    s_stream_run = true;
    xTaskCreate(stream_task, "stream", 4096, NULL, 3, &s_stream_task);
    return 0;
}

static int cmd_zero(int argc, char **argv)
{
    /* `zero reset` undoes the offset (back to 0); `zero` captures a new one. */
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        s_zero_mv = 0.0f;
        printf("zero reset to 0 mV\n");
        return 0;
    }
    int vo, vr; float mvd, a;
    measure(&vo, &vr, &mvd, &a);
    s_zero_mv = mvd;
    printf("zero set to %.2f mV  (readings are now relative to this)\n", s_zero_mv);
    return 0;
}

static int cmd_turns(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: turns <n>   (currently %d)\n", s_turns);
        return 0;
    }
    int n = atoi(argv[1]);
    if (n < 1) n = 1;
    s_turns = n;
    printf("turns = %d\n", s_turns);
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("HSTS016L current tester\n");
    printf("  Vout -> GPIO6 (ADC1_CH5),  Vref -> GPIO5 (ADC1_CH4)\n");
    printf("  sensitivity = %.2f mV/A   (20 A model)\n", SENS_MV_PER_A);
    printf("  turns = %d,  zero offset = %.2f mV\n", s_turns, s_zero_mv);
    printf("  eFuse mV calibration: %s\n", s_cali ? "on" : "off (using linear estimate)");
    printf("  amps = (Vout - Vref  -  zero) / %.2f / turns\n", SENS_MV_PER_A);
    return 0;
}

static void register_cmd(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t cmd = { .command = name, .help = help, .hint = NULL, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ---- Entry point ---- */

void app_main(void)
{
    /* Configure ADC1 for the two sensor channels. */
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_ID };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_CFG, .bitwidth = ADC_BITWIDTH_CFG };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, CH_VOUT, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, CH_VREF, &chan_cfg));

    /* Factory calibration for accurate raw->millivolts (optional). */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_ID, .atten = ADC_ATTEN_CFG, .bitwidth = ADC_BITWIDTH_CFG,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        s_cali = NULL;
    }
#endif

    /* Console over the native USB port (USB-Serial-JTAG). */
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "current>";
    repl_cfg.max_cmdline_length = 128;
    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl));

    esp_console_register_help_command();
    register_cmd("read",   "one reading: raw counts + mV + amps", cmd_read);
    register_cmd("stream", "stream [off] : live CSV t_ms,iout_raw,iref_raw,mv_diff,amps", cmd_stream);
    register_cmd("zero",   "zero [reset] : capture zero-current offset, or reset it to 0", cmd_zero);
    register_cmd("turns",  "turns <n> : wire passes through the sensor hole", cmd_turns);
    register_cmd("status", "show configuration", cmd_status);

    printf("\n=== HSTS016L current sensor tester ===\n");
    printf("Wiring: Vout->GPIO2, Vref->GPIO1, power 5V/GND.\n");
    printf("Commands: read | stream | zero | turns N | status | help\n");
    printf("Check: with NO current, `read` should show IOUT ~ IREF (~3100 counts).\n");
    printf("Then `zero`, apply current, and `stream` to watch amps.\n\n");

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
