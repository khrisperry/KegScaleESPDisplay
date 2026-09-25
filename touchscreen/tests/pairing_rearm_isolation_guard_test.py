#!/usr/bin/env python3
"""Prevent pairing repair from resetting Wi-Fi or disturbing another Scale slot."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
guard = (root / "main" / "pairing_guard.cpp").read_text(encoding="utf-8")
main = (root / "main" / "main.cpp").read_text(encoding="utf-8")

checks = {
    "pairing guard never disconnects station Wi-Fi":
        "esp_wifi_disconnect" not in guard,
    "pairing guard never reconnects station Wi-Fi":
        "esp_wifi_connect" not in guard,
    "pairing guard queues slot-specific reconnect":
        '"pairing_rearm"' in guard and
        'snprintf(action.body, sizeof(action.body), "{\\\"slot\\\":%u}", slot);' in guard and
        "xQueueSend(actions, &action, 0)" in guard,
    "failed queue remains retryable":
        "if (rearm_connection_for_pairing(slot, pending.host))" in guard,
    "main task handles pairing rearm":
        'if (!strcmp(a.kind, "pairing_rearm") && o)' in main,
    "inactive pairing does not steal active slot":
        "slot != active_scale_index" in main and
        "leaving active connection untouched" in main,
    "active pairing reconnect is slot-specific":
        "Re-arming Scale %u pairing connection without resetting Wi-Fi or the other Scale" in main and
        "connect_scale(slot);" in main,
}

failed = [name for name, passed in checks.items() if not passed]
if failed:
    raise SystemExit("Pairing rearm isolation guard failed: " + "; ".join(failed))

print("Touch pairing rearm isolation guard PASS")
