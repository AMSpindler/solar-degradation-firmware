/*
 * console_cmds.c — the text commands you type over USB. (See console_cmds.h.)
 *
 * ============================== HOW THIS WORKS ==============================
 * When you plug the board into your laptop and open the serial monitor, you get
 * a "clouds>" prompt. Whatever you type is split into words and handed to one of
 * the command functions below. ESP-IDF's "console" component does the typing,
 * editing, and history for us; we just register commands and provide the
 * functions that run them.
 *
 * Each command function looks like:  int cmd_xxx(int argc, char **argv)
 *   - argc = how many words were typed (including the command name)
 *   - argv = the array of those words as text strings
 *     e.g. typing `cal v point 100` gives argc=4 and
 *          argv[0]="cal", argv[1]="v", argv[2]="point", argv[3]="100"
 * Returning 0 means success.
 *
 * Special case: `plot` streams numbers continuously. We can't stream AND watch
 * for you to type `plot off` at the same time in one function, so `plot` starts
 * a separate background task that does the streaming; the command itself returns
 * right away, leaving the prompt free to accept `plot off`.
 * ===========================================================================
 */
#include "console_cmds.h"
#include "config.h"
#include "adc_sampler.h"
#include "rtc_clock.h"

#include <stdio.h>                   /* printf                                 */
#include <string.h>                 /* strcmp (compare text)                  */
#include <stdlib.h>                 /* atoi, strtof (text -> number)          */
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_console.h"            /* the REPL / command registration        */
#include "esp_heap_caps.h"         /* esp_get_free_heap_size (memory check)   */

static const char *TAG = "console";

/* ------------------------------------------------------------------------- */
/* plot — stream "V_calc,I_calc" so a serial plotter can graph it live        */
/* ------------------------------------------------------------------------- */

static TaskHandle_t   s_plot_task = NULL;   /* the streaming task (NULL=off)  */
static volatile bool  s_plot_run  = false;  /* flag the task watches to stop  */

/* This runs in its own background task. It pulls readings off the queue and
 * prints them as "voltage,current" lines until s_plot_run becomes false. */
static void plot_task(void *arg)
{
    (void)arg;
    /* A header line; many plotters use it to label the two graphs. */
    printf("V_calc,I_calc\n");
    SamplePacket p;
    while (s_plot_run) {
        /* xQueueReceive waits up to 100 ms for the next reading. If one
         * arrives, it's copied into `p` and we print it. The timeout means we
         * re-check s_plot_run regularly even if no data is flowing. */
        if (g_sample_queue &&
            xQueueReceive(g_sample_queue, &p, pdMS_TO_TICKS(100)) == pdTRUE) {
            float v, i;
            adc_sampler_apply_cal(&p, &v, &i);  /* raw -> volts/amps */
            printf("%.5f,%.5f\n", v, i);        /* 5 digits after the point */
        }
    }
    /* Clean up: forget our handle and delete this task (a task must delete
     * itself when its job is done; it cannot just "return"). */
    s_plot_task = NULL;
    vTaskDelete(NULL);
}

static int cmd_plot(int argc, char **argv)
{
    /* "plot off" stops; "plot" or "plot on" starts. */
    bool turn_off = (argc >= 2 && strcmp(argv[1], "off") == 0);

    if (turn_off) {
        s_plot_run = false;             /* the task sees this and exits itself */
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
    /* Start the streaming task on the network core (core 1) so it doesn't fight
     * the sampler on core 0. 4096 = stack size; 1 = low priority. */
    if (xTaskCreatePinnedToCore(plot_task, "plot", 4096, NULL, 1, &s_plot_task,
                                CORE_NETWORK) != pdPASS) {
        s_plot_run = false;
        printf("failed to start plot task\n");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* sample once — read one packet and print raw + millivolts + calibrated      */
/* ------------------------------------------------------------------------- */

static int cmd_sample(int argc, char **argv)
{
    (void)argc; (void)argv;             /* this command takes no extra words */
    SamplePacket p;
    if (adc_sampler_read_once(&p) != ESP_OK) {
        printf("read failed\n");
        return 1;
    }
    int v_diff = (int)p.voltage_raw - (int)p.aux_channels[0];
    int i_diff = (int)p.current_raw - (int)p.aux_channels[1];
    float v, i;
    adc_sampler_apply_cal(&p, &v, &i);

    /* Raw ADC counts (0..4095) — good for spotting wiring problems.
     * Each sensor shows its two legs and their difference. */
    printf("Vraw: VOUTP=%u VOUTN=%u diff=%d\n",
           p.voltage_raw, p.aux_channels[0], v_diff);
    printf("Iraw: IOUT=%u IREF=%u diff=%d\n",
           p.current_raw, p.aux_channels[1], i_diff);

    /* If factory calibration is available, also show millivolts. */
    int mvvp, mvvn, mvip, mvir;
    if (adc_sampler_raw_to_mv(p.voltage_raw, &mvvp) &&
        adc_sampler_raw_to_mv(p.aux_channels[0], &mvvn) &&
        adc_sampler_raw_to_mv(p.current_raw, &mvip) &&
        adc_sampler_raw_to_mv(p.aux_channels[1], &mvir)) {
        printf("mV  : V diff=%d   I diff=%d\n", mvvp - mvvn, mvip - mvir);
    }
    /* The final calibrated values in real units. */
    printf("calc: V=%.5f  I=%.5f\n", v, i);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* cal — two-point calibration                                                */
/* ------------------------------------------------------------------------- */
/*
 * The idea: feed the board two KNOWN values, measure the raw ADC reading at
 * each, and from those two (raw, known) points draw a straight line
 *     known = slope * raw + offset
 * Then any future raw reading can be turned into a real value with that line.
 *
 * Usage on the bench:
 *   cal v point 100      <- apply a known 100 V, capture point #0
 *   cal v point 500      <- apply a known 500 V, capture point #1
 *   cal v solve          <- compute + save slope/offset for voltage
 *   (same with `i` for the current sensor)
 */

/* Two pending points per channel, kept here until you `solve`. */
static float s_pts_x[CAL_CH_COUNT][2];   /* averaged raw reading             */
static float s_pts_y[CAL_CH_COUNT][2];   /* the known real value you told us */
static int   s_npts[CAL_CH_COUNT];       /* how many points captured so far  */

static const char *ch_name(int ch) { return ch == CAL_CH_VOLTAGE ? "V" : "I"; }

/* Print current calibration and any half-finished points. */
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

/* Capture one calibration point: average a bunch of raw readings and pair them
 * with the known value you typed. */
static int cal_capture_point(int ch, float known)
{
    float raw;
    /* Both are differential: voltage uses VOUTP-VOUTN, current uses Vout-Vref. */
    esp_err_t err = (ch == CAL_CH_VOLTAGE)
        ? adc_sampler_average_voltage_raw(ADC_READ_ONCE_AVG_N, &raw)
        : adc_sampler_average_current_raw(ADC_READ_ONCE_AVG_N, &raw);
    if (err != ESP_OK) {
        printf("averaging failed\n");
        return 1;
    }
    int idx = s_npts[ch];
    if (idx >= 2) {
        /* Already have two points — drop the oldest, keep the newest two, so
         * re-capturing "just works" without needing to clear first. */
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

/* Turn the two captured points into a slope+offset and save it. */
static int cal_solve(int ch)
{
    if (s_npts[ch] < 2) {
        printf("need 2 points to solve (%s has %d)\n", ch_name(ch), s_npts[ch]);
        return 1;
    }
    float x0 = s_pts_x[ch][0], y0 = s_pts_y[ch][0];
    float x1 = s_pts_x[ch][1], y1 = s_pts_y[ch][1];
    if (x1 == x0) {
        /* Same raw value for both points -> can't draw a line (divide by zero). */
        printf("degenerate points (equal raw); recapture\n");
        return 1;
    }
    cal_coeff_t c;
    /* Slope of a line through two points = rise / run. */
    c.slope  = (y1 - y0) / (x1 - x0);
    /* Offset: rearrange known = slope*raw + offset for one known point. */
    c.offset = y0 - c.slope * x0;
    if (adc_sampler_set_cal(ch, c) != ESP_OK) {   /* saves to NVS */
        printf("failed to persist calibration\n");
        return 1;
    }
    s_npts[ch] = 0;  /* clear the pending points now that we've used them */
    printf("%s calibrated & saved: slope=%.6g offset=%.6g\n",
           ch_name(ch), c.slope, c.offset);
    return 0;
}

/* The `cal` command itself just figures out which sub-command you typed and
 * calls the right helper above. */
static int cmd_cal(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "show") == 0) {   /* "cal" or "cal show" */
        cal_show();
        return 0;
    }
    if (strcmp(argv[1], "clear") == 0) {               /* "cal clear" */
        s_npts[0] = s_npts[1] = 0;
        printf("pending points cleared\n");
        return 0;
    }
    /* Which channel? "v" = voltage, "i" = current. */
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
        /* strtof turns the typed text (e.g. "100") into a float number. */
        return cal_capture_point(ch, strtof(argv[3], NULL));
    }
    if (strcmp(argv[2], "solve") == 0) {
        return cal_solve(ch);
    }
    printf("unknown cal subcommand: %s\n", argv[2]);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* reset cal — throw away calibration, back to value == raw                   */
/* ------------------------------------------------------------------------- */

static int cmd_reset(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "cal") != 0) {   /* must be exactly "reset cal" */
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
/* settime — set the clock (and save it to the DS3231)                        */
/* ------------------------------------------------------------------------- */

static int cmd_settime(int argc, char **argv)
{
    if (argc < 7) {   /* need year month day hour min sec */
        printf("usage: settime <year> <month> <day> <hour> <min> <sec>  (UTC)\n");
        return 1;
    }
    /* Fill a calendar struct from the typed numbers. atoi = text -> int.
     * tm_year counts from 1900 and tm_mon counts from 0, hence the -1900/-1. */
    struct tm t = {0};
    t.tm_year = atoi(argv[1]) - 1900;
    t.tm_mon  = atoi(argv[2]) - 1;
    t.tm_mday = atoi(argv[3]);
    t.tm_hour = atoi(argv[4]);
    t.tm_min  = atoi(argv[5]);
    t.tm_sec  = atoi(argv[6]);

    /* Set the ESP32 system clock... */
    time_t epoch = mktime(&t);
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    /* ...then copy it into the battery-backed DS3231 so it survives reboots. */
    if (rtc_clock_set_from_system() != ESP_OK) {
        printf("system clock set, but DS3231 write failed\n");
        return 1;
    }
    printf("time set on system + DS3231\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* status — a one-glance health summary                                       */
/* ------------------------------------------------------------------------- */

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("sampler   : %s @ %u Hz\n",
           adc_sampler_is_running() ? "running" : "stopped",
           adc_sampler_get_rate_hz());
    /* How many readings are waiting on the queue (vs. its capacity). If this is
     * pegged at the max, nothing is draining it. */
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
    /* "Free heap" = how much spare RAM is left. If this keeps shrinking over
     * time, something is leaking memory. */
    printf("free heap : %u bytes\n", (unsigned)esp_get_free_heap_size());
    return 0;
}

/* ------------------------------------------------------------------------- */
/* registration / starting the prompt                                         */
/* ------------------------------------------------------------------------- */

/* Small helper so registering each command is one tidy line below. */
static void register_cmd(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t cmd = { .command = name, .help = help, .hint = NULL, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* Called once from app_main(). Sets up the prompt over USB and registers every
 * command so the console knows what to do when you type. */
void console_start(void)
{
    esp_console_repl_t *repl = NULL;  /* REPL = Read-Eval-Print Loop (the prompt) */
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "clouds>";        /* the text shown before your cursor   */
    repl_cfg.max_cmdline_length = 256;  /* longest line you can type           */

    /* Use the ESP32-S3's built-in USB port (USB-Serial-JTAG) for the console. */
    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl));

    /* `help` lists all commands; the rest are ours. */
    esp_console_register_help_command();
    register_cmd("plot",   "plot [on|off] : stream V_calc,I_calc CSV for the serial plotter", cmd_plot);
    register_cmd("sample", "sample once : read one packet (raw + mV + calibrated)", cmd_sample);
    register_cmd("cal",    "cal [show|clear] | cal <v|i> point <known> | cal <v|i> solve", cmd_cal);
    register_cmd("reset",  "reset cal : restore identity calibration", cmd_reset);
    register_cmd("settime","settime Y M D h m s : set DS3231 + system clock (UTC)", cmd_settime);
    register_cmd("status", "status : sampler/queue/RTC/heap/calibration", cmd_status);

    /* Start listening. From here, typing at the prompt triggers our commands. */
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "console ready (USB-Serial-JTAG)");
}
