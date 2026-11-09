#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pixel_ui.h"

#define TC001_MQTT_PAYLOAD_MAX 4096
#define TC001_MQTT_TOPIC_MAX 128
#define TC001_MQTT_STALE_MS 35000

// A retained snapshot has no trustworthy age. Only live publications start freshness.
typedef struct {
    pixel_state_t state;
    bool connected, source_offline, have_live;
    int64_t last_live_ms;
} tc001_feed_t;
void tc001_feed_connect(tc001_feed_t *feed, bool connected);
bool tc001_feed_state(tc001_feed_t *feed, const char *json, size_t len, bool retained, int64_t now_ms);
void tc001_feed_availability(tc001_feed_t *feed, const char *value, size_t len);
bool tc001_feed_fresh(const tc001_feed_t *feed, int64_t now_ms);

// ESP-MQTT may fragment a publication; topic is normally supplied only in part one.
typedef struct {
    char topic[TC001_MQTT_TOPIC_MAX], payload[TC001_MQTT_PAYLOAD_MAX + 1];
    size_t length, expected;
    bool active, retained;
} tc001_mqtt_frame_t;
int tc001_mqtt_append(tc001_mqtt_frame_t *frame, const char *topic, int topic_len,
    const char *data, int len, int total, int offset, bool retained);
bool tc001_topic_base_valid(const char *base);
