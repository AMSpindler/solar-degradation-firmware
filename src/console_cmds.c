/*
 * console_cmds.c — see console_cmds.h.
 *
 * The `plot` stream runs in its own task so the REPL stays free to accept
 * `plot off`. All other commands run synchronously in the REPL task.
 */
#include "console_cmds.h"
#include "config.h"
#include "adc_sampler.h"
#include "rtc_clock.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_heap_caps.h"

static const char *TAG = "console";

/* ------------------------------------------------------------------------- */
/* plot                                                                      */
/* ------------------------------------------------------------------------- */

static TaskHandle_t   s_plot_task = NULL;
static volatile bool  s_plot_run  = false;

static void plot_task(void *arg)
{
    (void)arg;
    /* CSV header line for plotters that use it; numeric lines follow. */
    printf("V_calc,I_calc\n");
    SamplePacket p;
    while (s_plot_run) {
        if (g_sample_queue &&
            xQueueReceive(g_sample_queue, &p, pdMS_TO_TICKS(100)) == pdTRUE) {
            float v, i;
            adc_sampler_apply_cal(&p, &v, &i);
            printf("%.5f,%.5f\n", v, i);
        }
    }
    s_plot_task = NULL;
    vTaskDelete(NULL);
}

static int cmd_plot(int argc, char **argv)
{
    bool turn_off = (argc >= 2 && strcmp(argv[1], "off") == 0);

    if (turn_off) {
        s_plot_run = false;                 /* task observes flag and exits */
        printf("plot stopped\n");
        return 0;
    }
    if (s_plot_task != NULL) {
        printf("plot already running; use `plot off`\n");
        return 0;
    }
    if (!adc_sampler_is_running()) {
        printf("warning: sampler not running; no data will appear\n");
    }
    s_plot_run = true;
    if (xTaskCreatePinnedToCore(plot_task, "plot", 4096, NULL, 1, &s_plot_task,
                                CORE_NETWORK) != pdPASS) {
        s_plot_run = false;
        printf("failed to start plot task\n");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* sample once                                                               */
/* ------------------------------------------------------------------------- */

static int cmd_sample(int argc, char **argv)
{
    (void)argc; (void)argv;
    SamplePacket p;
    if (adc_sampler_read_once(&p) != ESP_OK) {
        printf("read failed\n");
        return 1;
    }
    int v_diff = (int)p.voltage_raw - (int)p.aux_channels[0];
    float v, i;
    adc_sampler_apply_cal(&p, &v, &i);

    printf("raw : VOUTP=%u VOUTN=%u diff=%d  I=%u  aux1=%u\n",
           p.voltage_raw, p.aux_channels[0], v_diff, p.current_raw, p.aux_channels[1]);

    int mvp, mvn, mvi;
    if (adc_sampler_raw_to_mv(p.voltage_raw, &mvp) &&
        adc_sampler_raw_to_mv(p.aux_channels[0], &mvn) &&
        adc_sampler_raw_to_mv(p.current_raw, &mvi)) {
        printf("mV  : VOUTP=%d VOUTN=%d diff=%d  I=%d\n", mvp, mvn, mvp - mvn, mvi);
    }
    printf("calc: V=%.5f  I=%.5f\n", v, i);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* cal                                                                       */
/* ------------------------------------------------------------------------- */

static float s_pts_x[CAL_CH_COUNT][2];   /* averaged raw */
static float s_pts_y[CAL_CH_COUNT][2];   /* known engineering value */
static int   s_npts[CAL_CH_COUNT];

static const char *ch_name(int ch) { return ch == CAL_CH_VOLTAGE ? "V" : "I"; }

static void cal_show(void)
{
    for (int ch = 0; ch < CAL_CH_COUNT; ch++) {
        cal_coeff_t c;
        adc_sampler_get_cal(ch, &c);
        printf("%s: slope=%.6g offset=%.6g  (pending points: %d)\n",
               ch_name(ch), c.slope, c.offset, s_npts[ch]);
        for (int k = 0; k < s_npts[ch]; k++) {
            printf("   point %d: raw=%.2f -> %.4f\n", k, s_pts_x[ch][k], s_pts_y[ch][k]);
        }
    }
}

static int cal_capture_point(int ch, float known)
{
    float raw;
    esp_err_t err = (ch == CAL_CH_VOLTAGE)
        ? adc_sampler_average_voltage_raw(ADC_READ_ONCE_AVG_N, &raw)
        : adc_sampler_average_current_raw(ADC_READ_ONCE_AVG_N, &raw);
    if (err != ESP_OK) {
        printf("averaging failed\n");
        return 1;
    }
    int idx = s_npts[ch];
    if (idx >= 2) {
        /* Keep the two most recent points: shift down. */
        s_pts_x[ch][0] = s_pts_x[ch][1];
        s_pts_y[ch][0] = s_pts_y[ch][1];
        idx = 1;
    }
    s_pts_x[ch][idx] = raw;
    s_pts_y[ch][idx] = known;
    s_npts[ch] = idx + 1;
    printf("%s point %d captured: raw=%.2f -> %.4f\n", ch_name(ch), idx, raw, known);
    return 0;
}

static int cal_solve(int ch)
{
    if (s_npts[ch] < 2) {
        printf("need 2 points to solve (%s has %d)\n", ch_name(ch), s_npts[ch]);
        return 1;
    }
    float x0 = s_pts_x[ch][0], y0 = s_pts_y[ch][0];
    float x1 = s_pts_x[ch][1], y1 = s_pts_y[ch][1];
    if (x1 == x0) {
        printf("degenerate points (equal raw); recapture\n");
        return 1;
    }
    cal_coeff_t c;
    c.slope  = (y1 - y0) / (x1 - x0);
    c.offset = y0 - c.slope * x0;
    if (adc_sampler_set_cal(ch, c) != ESP_OK) {
        printf("failed to persist calibration\n");
        return 1;
    }
    s_npts[ch] = 0;
    printf("%s calibrated & saved: slope=%.6g offset=%.6g\n",
           ch_name(ch), c.slope, c.offset);
    return 0;
}

static int cmd_cal(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "show") == 0) {
        cal_show();
        return 0;
    }
    if (strcmp(argv[1], "clear") == 0) {
        s_npts[0] = s_npts[1] = 0;
        printf("pending points cleared\n");
        return 0;
    }
    int ch;
    if (strcmp(argv[1], "v") == 0)      ch = CAL_CH_VOLTAGE;
    else if (strcmp(argv[1], "i") == 0) ch = CAL_CH_CURRENT;
    else {
        printf("usage: cal [show|clear] | cal <v|i> point <known> | cal <v|i> solve\n");
        return 1;
    }
    if (argc < 3) {
        printf("usage: cal <v|i> point <known> | cal <v|i> solve\n");
        return 1;
    }
    if (strcmp(argv[2], "point") == 0) {
        if (argc < 4) {
            printf("usage: cal %s point <known-value>\n", argv[1]);
            return 1;
        }
        return cal_capture_point(ch, strtof(argv[3], NULL));
    }
    if (strcmp(argv[2], "solve") == 0) {
        return cal_solve(ch);
    }
    printf("unknown cal subcommand: %s\n", argv[2]);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* reset cal                                                                 */
/* ------------------------------------------------------------------------- */

static int cmd_reset(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "cal") != 0) {
        printf("usage: reset cal\n");
        return 1;
    }
    if (adc_sampler_reset_cal() != ESP_OK) {
        printf("reset failed\n");
        return 1;
    }
    s_npts[0] = s_npts[1] = 0;
    printf("calibration reset to identity (slope=1, offset=0)\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* settime                                                                   */
/* ------------------------------------------------------------------------- */

static int cmd_settime(int argc, char **argv)
{
    if (argc < 7) {
        printf("usage: settime <year> <month> <day> <hour> <min> <sec>  (UTC)\n");
        return 1;
    }
    struct tm t = {0};
    t.tm_year = atoi(argv[1]) - 1900;
    t.tm_mon  = atoi(argv[2]) - 1;
    t.tm_mday = atoi(argv[3]);
    t.tm_hour = atoi(argv[4]);
    t.tm_min  = atoi(argv[5]);
    t.tm_sec  = atoi(argv[6]);

    time_t epoch = mktime(&t);
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    if (rtc_clock_set_from_system() != ESP_OK) {
        printf("system clock set, but DS3231 write failed\n");
        return 1;
    }
    printf("time set on system + DS3231\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* status                                                                    */
/* ------------------------------------------------------------------------- */

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("sampler   : %s @ %u Hz\n",
           adc_sampler_is_running() ? "running" : "stopped",
           adc_sampler_get_rate_hz());
    printf("queue     : %u / %d waiting\n",
           g_sample_queue ? (unsigned)uxQueueMessagesWaiting(g_sample_queue) : 0u,
           SAMPLE_QUEUE_LEN);

    struct tm rtc;
    if (rtc_clock_read(&rtc) == ESP_OK) {
        printf("RTC       : %04d-%02d-%02d %02d:%02d:%02d (DS3231)\n",
               rtc.tm_year + 1900, rtc.tm_mon + 1, rtc.tm_mday,
               rtc.tm_hour, rtc.tm_min, rtc.tm_sec);
    } else {
        printf("RTC       : read failed\n");
    }

    for (int ch = 0; ch < CAL_CH_COUNT; ch++) {
        cal_coeff_t c;
        adc_sampler_get_cal(ch, &c);
        printf("cal[%s]    : slope=%.6g offset=%.6g\n", ch_name(ch), c.slope, c.offset);
    }
    printf("free heap : %u bytes\n", (unsigned)esp_get_free_heap_size());
    return 0;
}

/* ------------------------------------------------------------------------- */
/* registration / REPL                                                       */
/* ------------------------------------------------------------------------- */

static void register_cmd(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t cmd = { .command = name, .help = help, .hint = NULL, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "clouds>";
    repl_cfg.max_cmdline_length = 256;

    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl));

    esp_console_register_help_command();
    register_cmd("plot",   "plot [on|off] : stream V_calc,I_calc CSV for the serial plotter", cmd_plot);
    register_cmd("sample", "sample once : read one packet (raw + mV + calibrated)", cmd_sample);
    register_cmd("cal",    "cal [show|clear] | cal <v|i> point <known> | cal <v|i> solve", cmd_cal);
    register_cmd("reset",  "reset cal : restore identity calibration", cmd_reset);
    register_cmd("settime","settime Y M D h m s : set DS3231 + system clock (UTC)", cmd_settime);
    register_cmd("status", "status : sampler/queue/RTC/heap/calibration", cmd_status);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "console ready (USB-Serial-JTAG)");
}
