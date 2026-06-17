/*
 * sample_packet.h — Firmware <-> pipeline wire contract.
 *
 * Both the firmware and the Python pipeline import this layout. The packet is
 * intentionally minimal (16 bytes): adding fields later is cheap, but extra
 * width costs Wi-Fi bandwidth now.
 *
 * NOTE on aux_channels during the HV lab phase: the AMC1311 voltage output is
 * differential, so aux_channels[0] carries the VOUTN raw reading (not a
 * photosensor). V_calc is computed downstream from (voltage_raw - aux[0]).
 */
#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define AUX_CHANNEL_COUNT 2

/* One sample, packed to exactly 16 bytes. */
typedef struct __attribute__((packed)) {
    uint64_t timestamp_us;                  /* esp_timer_get_time() at sample  */
    uint16_t voltage_raw;                   /* AMC1311 VOUTP raw ADC (0..4095) */
    uint16_t current_raw;                   /* ACS724 raw ADC (0..4095)        */
    uint16_t aux_channels[AUX_CHANNEL_COUNT]; /* aux[0]=VOUTN, aux[1]=spare    */
} SamplePacket;

_Static_assert(sizeof(SamplePacket) == 16, "SamplePacket must be exactly 16 bytes");

/*
 * Batch wire format (Phase B). The fixed header is followed immediately by
 * sample_count SamplePacket structs in the serialized buffer.
 */
typedef struct __attribute__((packed)) {
    uint32_t device_id;
    uint32_t sequence_num;                  /* monotonic; lets pipeline detect drops */
    uint16_t sample_count;
    uint16_t sample_rate_hz;
} SampleBatchHeader;

/*
 * Sample queue. Owned (created) by adc_sampler; carries individual
 * SamplePackets. Consumers (console `plot` in Phase A; wifi/sd in Phase B)
 * drain it. NULL until adc_sampler_init() runs.
 */
extern QueueHandle_t g_sample_queue;
