/*
 * wifi_transport.h — the "menu" for sending sample data over Wi-Fi.
 *
 * It connects to your lab Wi-Fi, then a background task batches readings and
 * ships them off-board. Two transports are supported (pick in config.h):
 *   - UDP  : fires packets straight at a PC + port. No broker, no setup.
 *   - MQTT : publishes to a Mosquitto broker (reliable, but needs setup +
 *            the esp-mqtt library vendored in components/).
 * Both can be on at once. Code lives in wifi_transport.c.
 */
#pragma once

#include "esp_err.h"

/* Connect to Wi-Fi and prepare the transport(s). Returns ESP_OK once Wi-Fi has
 * started (connection happens in the background). */
esp_err_t wifi_transport_init(void);

/* Subscribe to the sample stream and start the background sender task.
 * Call after adc_sampler_init() and before adc_sampler_start(). */
void wifi_transport_start(void);

/* True once Wi-Fi is associated and has an IP address. */
bool wifi_transport_is_connected(void);
