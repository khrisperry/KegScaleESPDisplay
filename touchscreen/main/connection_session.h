#pragma once

#include "esp_err.h"
#include <cstddef>
#include <cstdint>

struct SessionResetSnapshot {
  bool was_authenticated = false;
  bool was_online = false;
};

struct SessionHandshakeResult {
  bool pairing = false;
  bool send_auth = false;
  char approval_code[13] = {};
  uint32_t approval_seconds = 0;
};

SessionResetSnapshot connection_session_reset(uint8_t slot);

bool connection_session_send_secure(uint8_t slot, const char *plain,
                                    int64_t now_us);

esp_err_t connection_session_prepare_hello(uint8_t slot, bool paired,
                                           const uint8_t master[32],
                                           const char *firmware,
                                           char *hello, size_t hello_size);

esp_err_t connection_session_accept_handshake(
    uint8_t slot, bool saved_paired, bool pairing,
    const char *challenge_hex, const char *remote_public,
    int64_t now_us, bool seconds_present, double seconds_value,
    SessionHandshakeResult *result);
