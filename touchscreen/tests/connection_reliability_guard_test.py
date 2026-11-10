#!/usr/bin/env python3
"""V1.4.24 display reconnect and OTA-clock invariants."""
from pathlib import Path
from test_output import fail_exit, install_excepthook, pass_line

install_excepthook("V1.4.24 touchscreen/eInk reliability")
root = Path(__file__).resolve().parents[1]
main = (root/"main"/"main.cpp").read_text()
session = (root/"main"/"connection_session.cpp").read_text()
header = (root/"main"/"connection_transport.h").read_text()
checks = {
  "daily schedule conversion uses 64 bits": "(kDailyOtaCheckMs * (uint64_t)configTICK_RATE_HZ)" in main and
      "static_assert(kDailyOtaCheckRawTicks" in main,
  "no 32-bit 24h pdMS_TO_TICKS": "pdMS_TO_TICKS(kDailyOtaCheckMs)" not in main,
  "6 second authenticated ping": "current_time - c.last_ping > 6000000" in main,
  "stale detection allows 24 seconds": "kStateStaleUs = 24000000LL" in main,
  "handshake timeout bounded": "kHandshakeTimeoutUs = 22000000LL" in main and
      "current_time - c.handshake_started > kHandshakeTimeoutUs" in main,
  "handshake timer clears": "c.handshake_started = 0;" in session and
      "c.handshake_started = 0;" in main,
  "send hello timeout 3 seconds": "c.ws, hello, strlen(hello), pdMS_TO_TICKS(3000)" in main,
  "network timeout 10 seconds": "config.network_timeout_ms = 10000;" in main,
  "transport retirement still async": "retire_transport_async(slot" in main and
      "esp_websocket_client_destroy(c.ws)" not in main,
  "connection identity still generation guarded": "f.generation != c.generation.load()" in main,
  "session field explicit": "int64_t handshake_started = 0;" in header,
}
failed=[k for k,v in checks.items() if not v]
if failed: fail_exit("Display reliability: " + "; ".join(failed))
pass_line("V1.4.24 reconnect + safe 24h OTA scheduling")
