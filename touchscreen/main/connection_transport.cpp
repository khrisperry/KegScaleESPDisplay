#include "connection_transport.h"

#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cstring>

namespace {
const char *TAG = "wifi_touchscreen";
ScaleConnection connections[2];
QueueHandle_t frames;
QueueHandle_t retired_transports;
TaskHandle_t transport_retirement_task_handle;

struct RetiredTransport {
  esp_websocket_client_handle_t handle;
  uint8_t slot;
  uint32_t generation;
};

void transport_retirement_task(void *) {
  RetiredTransport retired{};
  for (;;) {
    if (xQueueReceive(retired_transports, &retired, portMAX_DELAY) != pdTRUE)
      continue;
    if (!retired.handle)
      continue;

    ESP_LOGI(TAG,
             "Retiring scale %u WebSocket off main task generation=%lu",
             (unsigned)(retired.slot + 1),
             (unsigned long)retired.generation);
    esp_websocket_client_stop(retired.handle);
    esp_websocket_client_destroy(retired.handle);
    connection_for(retired.slot).retirement_pending = false;
    ESP_LOGI(TAG,
             "Retired scale %u WebSocket generation=%lu; reconnect may proceed",
             (unsigned)(retired.slot + 1),
             (unsigned long)retired.generation);
  }
}
} // namespace

ScaleConnection &connection_for(uint8_t slot) {
  return connections[slot == 1 ? 1 : 0];
}

const ScaleConnection &connection_for_const(uint8_t slot) {
  return connections[slot == 1 ? 1 : 0];
}

void connection_transport_init() {
  frames = xQueueCreate(8, sizeof(Frame));
  retired_transports = xQueueCreate(8, sizeof(RetiredTransport));
  configASSERT(frames && retired_transports);

  BaseType_t created =
      xTaskCreate(transport_retirement_task, "ws_retire", 4096, nullptr, 4,
                  &transport_retirement_task_handle);
  configASSERT(created == pdPASS);
}

bool connection_receive_frame(Frame *frame, TickType_t timeout) {
  return frame && xQueueReceive(frames, frame, timeout) == pdTRUE;
}

bool retire_transport_async(uint8_t slot,
                            esp_websocket_client_handle_t handle,
                            uint32_t generation) {
  if (!handle)
    return true;

  RetiredTransport retired{};
  retired.handle = handle;
  retired.slot = slot;
  retired.generation = generation;
  auto &c = connection_for(slot);
  c.retirement_pending = true;
  if (xQueueSend(retired_transports, &retired, 0) != pdTRUE) {
    c.retirement_pending = false;
    ESP_LOGE(TAG,
             "Transport retirement queue full for scale %u generation=%lu; deferring reconnect",
             (unsigned)(slot + 1), (unsigned long)generation);
    return false;
  }
  return true;
}

void socket_event(void *arg, esp_event_base_t, int32_t event, void *data) {
  const uint32_t token =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  const uint8_t slot = token & 1U;
  const uint32_t event_generation = token >> 1;
  if (slot > 1)
    return;
  auto &c = connection_for(slot);
  const uint32_t current_generation = c.generation.load();
  if (event_generation != current_generation) {
    ESP_LOGD(TAG,
             "Ignoring stale scale %u WebSocket event=%ld generation=%lu current=%lu",
             (unsigned)(slot + 1), (long)event,
             (unsigned long)event_generation,
             (unsigned long)current_generation);
    return;
  }

  Frame f{};
  f.slot = slot;
  f.generation = event_generation;
  if (event == WEBSOCKET_EVENT_CONNECTED) {
    ESP_LOGI(TAG, "Scale %u WebSocket transport connected to %s generation=%lu",
             (unsigned)(slot + 1), c.uri,
             (unsigned long)event_generation);
    f.kind = 1;
    if (xQueueSend(frames, &f, 0) != pdTRUE)
      ESP_LOGW(TAG, "Frame queue full while reporting scale %u connection",
               (unsigned)(slot + 1));
  } else if (event == WEBSOCKET_EVENT_DISCONNECTED ||
             event == WEBSOCKET_EVENT_ERROR) {
    const bool is_error = event == WEBSOCKET_EVENT_ERROR;
    ESP_LOGW(TAG,
             "Scale %u WebSocket transport %s from %s generation=%lu%s",
             (unsigned)(slot + 1),
             is_error ? "error" : "disconnected",
             c.uri,
             (unsigned long)event_generation,
             is_error ? "; scheduling reconnect" : "");

    const uint32_t already_reported =
        c.disconnect_reported_generation.exchange(event_generation);
    if (already_reported != event_generation) {
      f.kind = 2;
      if (xQueueSend(frames, &f, 0) != pdTRUE)
        ESP_LOGW(TAG,
                 "Frame queue full while reporting scale %u WebSocket %s",
                 (unsigned)(slot + 1),
                 is_error ? "error" : "disconnect");
    } else {
      ESP_LOGD(TAG,
               "Scale %u generation=%lu disconnect already queued; suppressing duplicate event",
               (unsigned)(slot + 1), (unsigned long)event_generation);
    }
    c.assembly = {};
  } else if (event == WEBSOCKET_EVENT_DATA) {
    auto *d = static_cast<esp_websocket_event_data_t *>(data);
    if (d->op_code != 1 && d->op_code != 2)
      return;
    if (d->payload_offset == 0) {
      c.assembly = {};
      c.assembly.slot = slot;
      c.assembly.generation = event_generation;
      c.assembly.kind = d->op_code == 1 ? 3 : 4;
    }
    if (d->payload_len > CL_MAX_FRAME || d->payload_offset < 0 ||
        d->data_len < 0 || (size_t)d->payload_offset != c.assembly.length ||
        c.assembly.length + d->data_len > CL_MAX_FRAME) {
      ESP_LOGW(TAG,
               "Rejected scale %u WebSocket fragment: opcode=%d payload_len=%d offset=%d data_len=%d assembled=%u",
               (unsigned)(slot + 1), d->op_code, d->payload_len,
               d->payload_offset, d->data_len, (unsigned)c.assembly.length);
      return;
    }
    memcpy(c.assembly.bytes + c.assembly.length, d->data_ptr, d->data_len);
    c.assembly.length += d->data_len;
    if (c.assembly.length == (size_t)d->payload_len) {
      ESP_LOGD(TAG,
               "Complete scale %u WebSocket message received: opcode=%d len=%u",
               (unsigned)(slot + 1), d->op_code,
               (unsigned)c.assembly.length);
      if (xQueueSend(frames, &c.assembly, 0) != pdTRUE)
        ESP_LOGW(TAG, "Frame queue full; dropping scale %u WebSocket message",
                 (unsigned)(slot + 1));
    }
  }
}
