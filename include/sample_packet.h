/*
 * sample_packet.h — defines the exact shape of one sensor reading.
 *
 * ============================ WHAT IS A HEADER? ============================
 * A ".h" header file describes things (data shapes and function names) that
 * other ".c" files want to share. A ".c" file #includes a header to learn
 * "this thing exists and looks like this" without copy-pasting code.
 *
 * This particular header is the agreed "contract" between the firmware and the
 * Python program on the laptop that will later read the data: both sides agree
 * a reading is exactly these 16 bytes in this order. Keeping it small saves
 * Wi-Fi bandwidth when we stream lots of readings.
 *
 * NOTE for the HV lab phase: the AMC1311 voltage sensor is "differential", so
 * we temporarily store its second pin (VOUTN) in aux_channels[0] instead of a
 * photosensor. The real voltage is (voltage_raw - aux_channels[0]).
 * ==========================================================================
 */
#pragma once   /* "only include me once per file, even if asked twice" */

#include <stdint.h>              /* fixed-size number types like uint16_t   */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"      /* the "queue" / conveyor-belt type        */

#define AUX_CHANNEL_COUNT 2

/*
 * One sample. A "struct" bundles several values into one named package.
 *
 * The number types say exactly how big each field is:
 *   uint64_t = unsigned 64-bit (8 bytes), uint16_t = unsigned 16-bit (2 bytes).
 * Add them up: 8 + 2 + 2 + (2*2) = 16 bytes.
 *
 * __attribute__((packed)) tells the compiler "do NOT add hidden padding bytes
 * between fields for alignment." That guarantees the layout is exactly 16 bytes
 * so the laptop side can rely on it.
 */
typedef struct __attribute__((packed)) {
    uint64_t timestamp_us;                  /* microseconds since boot         */
    uint16_t voltage_raw;                   /* AMC1311 VOUTP raw ADC (0..4095) */
    uint16_t current_raw;                   /* ACS724 raw ADC (0..4095)        */
    uint16_t aux_channels[AUX_CHANNEL_COUNT]; /* aux[0]=VOUTN, aux[1]=spare    */
} SamplePacket;

/* A compile-time safety check: if SamplePacket is ever NOT 16 bytes (say
 * someone adds a field), the build fails right here with this message instead
 * of silently breaking the contract with the laptop. */
_Static_assert(sizeof(SamplePacket) == 16, "SamplePacket must be exactly 16 bytes");

/*
 * The batch header (Phase B). When we stream over Wi-Fi we send many samples at
 * once: first this small header, then sample_count SamplePackets back to back.
 * sequence_num lets the laptop notice if a batch went missing.
 */
typedef struct __attribute__((packed)) {
    uint32_t device_id;
    uint32_t sequence_num;
    uint16_t sample_count;
    uint16_t sample_rate_hz;
} SampleBatchHeader;

/*
 * The shared sample queue (the "conveyor belt"). `extern` means "this variable
 * is defined in some .c file — here is just its name so others can use it." It
 * actually lives in adc_sampler.c. It is NULL until adc_sampler_init() runs.
 */
extern QueueHandle_t g_sample_queue;
