#pragma once

#include "app.h"
#include "controller_link.h"
#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

struct Frame {
  uint8_t slot;
  uint32_t generation;
  int kind;
  size_t length;
  uint8_t bytes[CL_MAX_FRAME + 1];
};

struct ScaleConnection {
  cl_session_t link{};
  esp_websocket_client_handle_t ws = nullptr;
  Frame assembly{};
  char own_public[131] = {};
  char uri[180] = {};
  char pending_op[32] = {};
  uint8_t client_nonce[32] = {};
  uint32_t request_id = 0;
  uint32_t pending_id = 0;
  uint32_t calibration_session_id = 0;
  int64_t last_state = 0;
  int64_t pending_since = 0;
  int64_t last_ping = 0;
  int64_t last_reading = 0;
  int64_t last_age_update = 0;
  int64_t next_connection_attempt = 0;
  int64_t pairing_deadline_us = 0;
  int64_t cancel_pairing_deadline_us = 0;
  bool authenticated = false;
  bool traffic_ready = false;
  bool retry_connection = true;
  std::atomic<uint32_t> generation{0};
  std::atomic<uint32_t> disconnect_reported_generation{0};
  std::atomic<bool> retirement_pending{false};
  State state{};
};

ScaleConnection &connection_for(uint8_t slot);
const ScaleConnection &connection_for_const(uint8_t slot);

void connection_transport_init();
bool connection_receive_frame(Frame *frame, TickType_t timeout);
bool retire_transport_async(uint8_t slot,
                            esp_websocket_client_handle_t handle,
                            uint32_t generation);
void socket_event(void *arg, esp_event_base_t base, int32_t event, void *data);
