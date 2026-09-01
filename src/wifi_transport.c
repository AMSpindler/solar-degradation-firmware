/*
 * wifi_transport.c — send sample batches over Wi-Fi. (See wifi_transport.h.)
 *
 * ============================== HOW IT WORKS ==============================
 * 1. Connect to the lab Wi-Fi as a normal client ("station" / STA mode).
 *    If the connection drops, we retry with "exponential backoff": wait 1s,
 *    then 2s, 4s, ... up to 30s, so we don't hammer the router.
 * 2. A background task pulls readings off our subscriber queue, groups them
 *    into a "batch" (a small header + many SamplePackets), and sends the batch:
 *       - over UDP  (always available; a PC listens on a port), and/or
 *       - over MQTT (to a Mosquitto broker), if TRANSPORT_USE_MQTT is on.
 *
 * Which transports are active is set in config.h. SD logging is separate, so
 * if Wi-Fi is down the SD card still has the full record.
 * =========================================================================
 */
#include "wifi_transport.h"
#include "config.h"
#include "adc_sampler.h"
#include "sample_packet.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"      /* UDP sockets */

#if WIFI_ENTERPRISE
#include "esp_eap_client.h"    /* WPA2-Enterprise (eduroam/MWireless) */
#endif

#if TRANSPORT_USE_MQTT
#include "mqtt_client.h"
#endif

#if THINGSPEAK_ENABLE
#include "esp_http_client.h"   /* battery-test heartbeat over HTTP (port 80) */
#endif

static const char *TAG = "wifi_tx";

/* Wi-Fi connection state. "volatile" because it's changed in event callbacks
 * and read in the sender task — it tells the compiler not to cache it. */
static volatile bool s_wifi_connected = false;

/* Exponential-backoff state for reconnecting. */
static esp_timer_handle_t s_reconnect_timer = NULL;
static uint32_t           s_backoff_ms      = WIFI_RECONNECT_MIN_MS;

/* The subscriber queue the sampler copies readings into for us. */
static QueueHandle_t s_queue = NULL;
static uint32_t      s_seq   = 0;   /* batch sequence number (lets PC spot drops) */

/* Transmit buffer: one batch = header + up to BATCH_SAMPLES readings.
 * aligned(4) keeps the header fields nicely aligned. */
static __attribute__((aligned(4)))
uint8_t s_txbuf[sizeof(SampleBatchHeader) + BATCH_SAMPLES * sizeof(SamplePacket)];

#if TRANSPORT_USE_UDP
static int                s_udp_sock = -1;
static struct sockaddr_in s_udp_dest;
#endif

#if TRANSPORT_USE_MQTT
static esp_mqtt_client_handle_t s_mqtt = NULL;
static volatile bool            s_mqtt_connected = false;
static char                     s_mqtt_topic[64];
static char                     s_mqtt_status_topic[64];   /* presence: online/offline */

/* esp-mqtt tells us when the broker connection comes and goes. */
static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)args; (void)base; (void)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT broker connected");
        /* "Birth" message: announce we're online. RETAINED so a PC that
         * subscribes later still sees it. This overwrites any stale "offline"
         * the broker published from our Last Will on a previous drop. */
        esp_mqtt_client_publish(s_mqtt, s_mqtt_status_topic,
                                MQTT_STATUS_ONLINE, 0, MQTT_QOS, /*retain=*/1);
#if WIFI_TEST_ENABLE
        /* Listen for the round-trip test messages (mosquitto_pub -t wifi/esp/in). */
        esp_mqtt_client_subscribe(s_mqtt, WIFI_TEST_TOPIC_IN, MQTT_QOS);
        ESP_LOGI(TAG, "wifi test: subscribed to %s", WIFI_TEST_TOPIC_IN);
#endif
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT broker disconnected");
        break;
#if WIFI_TEST_ENABLE
    case MQTT_EVENT_DATA: {
        /* Something arrived on WIFI_TEST_TOPIC_IN. topic/data are NOT null-
         * terminated, so print with explicit lengths. */
        esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
        ESP_LOGI(TAG, "wifi test  PC -> ESP: %.*s = %.*s",
                 event->topic_len, event->topic, event->data_len, event->data);
        break;
    }
#endif
    default:
        break;
    }
}
#endif

/* ------------------------------------------------------------------------- */
/* Wi-Fi connect / reconnect                                                 */
/* ------------------------------------------------------------------------- */

/* Called by the backoff timer: time to try connecting again. */
static void reconnect_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "reconnecting to Wi-Fi...");
    esp_wifi_connect();
}

/* One handler for both Wi-Fi events and IP events. */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();                         /* first connect attempt */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        /* reason code tells us WHY: 15=handshake timeout (bad password/PSK),
         * 23=802.1X auth failed (bad EAP creds), 2/4=auth/assoc expire,
         * 200/201=beacon/no-AP (out of range). */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d); retry in %lu ms",
                 d ? d->reason : -1, (unsigned long)s_backoff_ms);
        /* Schedule a retry, then grow the wait (capped) for next time. */
        esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
        s_backoff_ms *= 2;
        if (s_backoff_ms > WIFI_RECONNECT_MAX_MS) {
            s_backoff_ms = WIFI_RECONNECT_MAX_MS;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_connected = true;
        s_backoff_ms = WIFI_RECONNECT_MIN_MS;       /* reset backoff on success */
    }
}

/* ------------------------------------------------------------------------- */
/* Setup                                                                     */
/* ------------------------------------------------------------------------- */

esp_err_t wifi_transport_init(void)
{
    /* Networking plumbing. The default event loop was already created in main. */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* Fill in the network name. For a normal password network we also set the
     * password here; for WPA2-Enterprise the login is configured separately below. */
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
#if !WIFI_ENTERPRISE
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
#endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

#if WIFI_ENTERPRISE
    /* WPA2-Enterprise (802.1X, e.g. eduroam/MWireless): supply the campus
     * identity/username/password, then switch the station into enterprise mode. */
    ESP_ERROR_CHECK(esp_eap_client_set_identity((const uint8_t *)EAP_IDENTITY, strlen(EAP_IDENTITY)));
    ESP_ERROR_CHECK(esp_eap_client_set_username((const uint8_t *)EAP_USERNAME, strlen(EAP_USERNAME)));
    ESP_ERROR_CHECK(esp_eap_client_set_password((const uint8_t *)EAP_PASSWORD, strlen(EAP_PASSWORD)));
    ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());
    ESP_LOGI(TAG, "WPA2-Enterprise enabled for SSID %s", WIFI_SSID);
#endif

    /* Create the (stopped) backoff timer used for reconnect attempts. */
    const esp_timer_create_args_t targs = { .callback = reconnect_cb, .name = "wifi_reconn" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_wifi_start());              /* triggers STA_START event */

#if TRANSPORT_USE_UDP
    /* Prepare the UDP socket + destination address (the lab PC). The socket can
     * exist before Wi-Fi is up; we just don't send until connected. */
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "UDP socket create failed");
    } else {
        memset(&s_udp_dest, 0, sizeof(s_udp_dest));
        s_udp_dest.sin_family = AF_INET;
        s_udp_dest.sin_port   = htons(UDP_DEST_PORT);
        inet_pton(AF_INET, UDP_DEST_IP, &s_udp_dest.sin_addr);
        ESP_LOGI(TAG, "UDP -> %s:%d", UDP_DEST_IP, UDP_DEST_PORT);
    }
#endif

#if TRANSPORT_USE_MQTT
    snprintf(s_mqtt_topic, sizeof(s_mqtt_topic), MQTT_TOPIC_FMT, (unsigned long)DEVICE_ID);
    snprintf(s_mqtt_status_topic, sizeof(s_mqtt_status_topic),
             MQTT_STATUS_TOPIC_FMT, (unsigned long)DEVICE_ID);
    /* Last Will & Testament: the broker publishes this RETAINED "offline" to the
     * status topic on our behalf if we disconnect ungracefully (Wi-Fi/power/crash).
     * esp-mqtt copies these strings internally, so the static buffer is safe. */
    esp_mqtt_client_config_t mcfg = {
        .broker.address.uri              = MQTT_BROKER_URI,
        .session.last_will.topic         = s_mqtt_status_topic,
        .session.last_will.msg           = MQTT_STATUS_OFFLINE,
        .session.last_will.msg_len       = 0,   /* 0 = derive length from string */
        .session.last_will.qos           = MQTT_QOS,
        .session.last_will.retain        = 1,
    };
    s_mqtt = esp_mqtt_client_init(&mcfg);
    if (s_mqtt) {
        /* esp-mqtt handles its own reconnects once started. */
        esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(s_mqtt);
        ESP_LOGI(TAG, "MQTT -> %s topic %s", MQTT_BROKER_URI, s_mqtt_topic);
    }
#endif

    return ESP_OK;
}

bool wifi_transport_is_connected(void) { return s_wifi_connected; }

bool wifi_transport_mqtt_is_connected(void)
{
#if TRANSPORT_USE_MQTT
    return s_mqtt_connected;
#else
    return false;
#endif
}

/* ------------------------------------------------------------------------- */
/* Sender task: batch readings and ship them                                 */
/* ------------------------------------------------------------------------- */

static void send_batch(const uint8_t *buf, size_t len)
{
#if TRANSPORT_USE_UDP
    if (s_udp_sock >= 0 && s_wifi_connected) {
        sendto(s_udp_sock, buf, len, 0,
               (struct sockaddr *)&s_udp_dest, sizeof(s_udp_dest));
    }
#endif
#if TRANSPORT_USE_MQTT
    if (s_mqtt && s_mqtt_connected) {
        esp_mqtt_client_publish(s_mqtt, s_mqtt_topic, (const char *)buf, len, MQTT_QOS, 0);
    }
#endif
}

static void sender_task(void *arg)
{
    (void)arg;
    SampleBatchHeader *hdr = (SampleBatchHeader *)s_txbuf;
    SamplePacket *samps = (SamplePacket *)(s_txbuf + sizeof(SampleBatchHeader));

    for (;;) {
        /* Wait (up to 1 s) for the first reading of a batch. */
        int count = 0;
        if (xQueueReceive(s_queue, &samps[0], pdMS_TO_TICKS(1000)) == pdTRUE) {
            count = 1;
            /* Grab as many more as are ready, without waiting, up to a full batch. */
            while (count < BATCH_SAMPLES &&
                   xQueueReceive(s_queue, &samps[count], 0) == pdTRUE) {
                count++;
            }
        }
        if (count == 0) {
            continue;   /* nothing arrived (sampler stopped?) — loop again */
        }

        hdr->device_id      = DEVICE_ID;
        hdr->sequence_num   = s_seq++;
        hdr->sample_count   = (uint16_t)count;
        hdr->sample_rate_hz = adc_sampler_get_rate_hz();

        size_t len = sizeof(SampleBatchHeader) + (size_t)count * sizeof(SamplePacket);
        send_batch(s_txbuf, len);
    }
}

#if TRANSPORT_USE_MQTT && WIFI_TEST_ENABLE
/* Round-trip self-test: publish a counter to WIFI_TEST_TOPIC_OUT once a second
 * while connected, so `mosquitto_sub -t wifi/esp/out -v` shows the link is live.
 * The matching receive side is MQTT_EVENT_DATA in the handler above. */
static void wifi_test_task(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    char buf[24];
    for (;;) {
        if (s_mqtt && s_mqtt_connected) {
            int len = snprintf(buf, sizeof(buf), "%lu", (unsigned long)n);
            /* retain=1: broker keeps the last counter, so if the battery dies
             * overnight the final value is still there to read back. */
            esp_mqtt_client_publish(s_mqtt, WIFI_TEST_TOPIC_OUT, buf, len, MQTT_QOS, 1);
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_TEST_PERIOD_MS));
    }
}
#endif

#if THINGSPEAK_ENABLE
/* Battery-test heartbeat: GET an incrementing counter to ThingSpeak every
 * THINGSPEAK_PERIOD_MS. ThingSpeak timestamps and charts each point; when the
 * battery dies the last point marks the time. Uses plain HTTP (port 80), which
 * the campus firewall allows — unlike MQTT's port 1883. */
static void thingspeak_task(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    char url[256];
    for (;;) {
        if (s_wifi_connected) {
            snprintf(url, sizeof(url),
                     "http://%s/update?api_key=%s&field1=%lu",
                     THINGSPEAK_HOST, THINGSPEAK_API_KEY, (unsigned long)n);
            esp_http_client_config_t cfg = {
                .url        = url,
                .method     = HTTP_METHOD_GET,
                .timeout_ms = 8000,
            };
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            esp_err_t err = esp_http_client_perform(c);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "thingspeak field1=%lu -> HTTP %d",
                         (unsigned long)n, esp_http_client_get_status_code(c));
                n++;
            } else {
                ESP_LOGW(TAG, "thingspeak GET failed: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(c);
        }
        vTaskDelay(pdMS_TO_TICKS(THINGSPEAK_PERIOD_MS));
    }
}
#endif

void wifi_transport_start(void)
{
    s_queue = xQueueCreate(CONSUMER_QUEUE_LEN, sizeof(SamplePacket));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "queue alloc failed; Wi-Fi sender not started");
        return;
    }
    adc_sampler_subscribe(s_queue);
    /* Run on the network core (core 1), away from the sampler on core 0. */
    xTaskCreatePinnedToCore(sender_task, "wifi_tx", 4096, NULL, PRIO_WIFI, NULL, CORE_NETWORK);
#if TRANSPORT_USE_MQTT && WIFI_TEST_ENABLE
    xTaskCreatePinnedToCore(wifi_test_task, "wifi_test", 3072, NULL, PRIO_WIFI, NULL, CORE_NETWORK);
#endif
#if THINGSPEAK_ENABLE
    xTaskCreatePinnedToCore(thingspeak_task, "thingspeak", 8192, NULL, PRIO_WIFI, NULL, CORE_NETWORK);
#endif
}
