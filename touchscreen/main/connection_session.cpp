#include "connection_session.h"

#include "connection_transport.h"
#include "controller_link.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
const char *TAG = "wifi_touchscreen";
}

SessionResetSnapshot connection_session_reset(uint8_t slot) {
  auto &c = connection_for(slot);
  SessionResetSnapshot result{};
  result.was_authenticated = c.authenticated;
  result.was_online = c.state.online;

  c.authenticated = false;
  c.traffic_ready = false;
  c.state.online = false;
  c.last_state = 0;
  cl_clear(&c.link);
  return result;
}

bool connection_session_send_secure(uint8_t slot, const char *plain,
                                    int64_t now_us) {
  auto &c = connection_for(slot);
  uint8_t out[CL_MAX_FRAME];
  size_t len = 0;

  /*
   * cl_seal() advances tx_seq. Stage that advancement in a copy and only
   * commit it after the complete WebSocket frame is accepted for transport.
   * A mutex/transport failure must never consume an encrypted sequence number.
   */
  cl_session_t staged = c.link;
  esp_err_t e = cl_seal(&staged, plain, out, &len);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "Could not encrypt scale %u WebSocket message: %s",
             (unsigned)(slot + 1), esp_err_to_name(e));
    return false;
  }
  if (!c.ws || !esp_websocket_client_is_connected(c.ws))
    return false;

  int sent = esp_websocket_client_send_bin(
      c.ws, reinterpret_cast<char *>(out), len, pdMS_TO_TICKS(1000));
  if (sent != (int)len) {
    ESP_LOGW(TAG,
             "Scale %u encrypted WebSocket send incomplete: sent=%d expected=%u",
             (unsigned)(slot + 1), sent, (unsigned)len);
    c.traffic_ready = false;
    c.retry_connection = true;
    c.next_connection_attempt = now_us;
    return false;
  }

  c.link.tx_seq = staged.tx_seq;
  return true;
}

esp_err_t connection_session_prepare_hello(uint8_t slot, bool paired,
                                           const uint8_t master[32],
                                           const char *firmware,
                                           char *hello, size_t hello_size) {
  auto &c = connection_for(slot);
  if (!hello || !hello_size || !firmware)
    return ESP_ERR_INVALID_ARG;

  char nonce_hex[65];
  esp_fill_random(c.client_nonce, 32);
  cl_hex(c.client_nonce, 32, nonce_hex);

  if (paired) {
    if (!master)
      return ESP_ERR_INVALID_ARG;
    memcpy(c.link.master, master, 32);
    snprintf(hello, hello_size,
             "{\"type\":\"hello\",\"protocol\":1,\"firmware\":\"%s\",\"nonce\":\"%s\"}",
             firmware, nonce_hex);
    return ESP_OK;
  }

  esp_err_t e = cl_keypair(&c.link, c.own_public);
  if (e != ESP_OK)
    return e;

  snprintf(hello, hello_size,
           "{\"type\":\"hello\",\"protocol\":1,\"firmware\":\"%s\",\"public\":\"%s\",\"nonce\":\"%s\"}",
           firmware, c.own_public, nonce_hex);
  return ESP_OK;
}

esp_err_t connection_session_accept_handshake(
    uint8_t slot, bool saved_paired, bool pairing,
    const char *challenge_hex, const char *remote_public,
    int64_t now_us, bool seconds_present, double seconds_value,
    SessionHandshakeResult *result) {
  if (!result || !challenge_hex)
    return ESP_ERR_INVALID_ARG;
  *result = {};
  result->pairing = pairing;

  auto &c = connection_for(slot);
  uint8_t challenge[32];
  if (!cl_unhex(challenge_hex, challenge, 32))
    return ESP_FAIL;

  esp_err_t e = ESP_OK;
  if (pairing) {
    e = cl_agree(&c.link, remote_public ? remote_public : "",
                 remote_public ? remote_public : "",
                 c.own_public, result->approval_code);
    if (e == ESP_OK) {
      const int64_t remaining = c.pairing_deadline_us - now_us;
      const double clamped =
          std::max(0.0, std::min(300.0, seconds_value));
      result->approval_seconds =
          seconds_present
              ? static_cast<uint32_t>(clamped)
              : (remaining > 0
                     ? static_cast<uint32_t>(
                           (remaining + 999999) / 1000000)
                     : 0);
    }
  } else if (!saved_paired) {
    return ESP_ERR_INVALID_STATE;
  }

  if (e != ESP_OK)
    return e;

  e = cl_start(&c.link, challenge, c.client_nonce, false);
  c.traffic_ready = e == ESP_OK;
  result->send_auth = e == ESP_OK && !pairing;
  return e;
}
