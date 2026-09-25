#!/usr/bin/env python3
"""Guard e-paper reliable-command BLE wiring and completion boundaries."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
client = (root / "components/ble_client/ble_client.c").read_text(encoding="utf-8")
header = (root / "components/ble_client/include/ble_client.h").read_text(encoding="utf-8")
main = (root / "main/main.c").read_text(encoding="utf-8")

checks = {
    "ACK UUID is discovered":
        "s_display_command_ack_uuid" in client and
        "display_command_ack_handle" in client,
    "control packet exposes command id":
        "uint16_t command_id;" in client and
        "control.command_id" in client,
    "replacement flag remains decoded":
        "DISPLAY_CONTROL_REPLACE" in client and
        "replacement_requested" in client,
    "client exposes completion ACK API":
        "ble_client_acknowledge_control" in header and
        "ble_client_acknowledge_control(" in client,
    "firmware capability is reported before control read":
        client.index("display_info_handle != 0") <
        client.index("display_control_handle != 0"),
    "reliable control read waits for capability report":
        "Do not read a\n             * reliable command until capability reporting succeeds." in client,
    "force refresh ACK follows successful render":
        "Full refresh completed, but command ACK failed" in main,
    "unpair ACK precedes local bond deletion":
        main.index("ble_client_acknowledge_control(") <
        main.index("ble_client_forget_peer(", main.index("handle_unpair_request")),
    "failed unpair ACK preserves pairing":
        "keeping local bond so command id=" in main,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("Display command ACK guard failed: " + "; ".join(failed))

print("Display command ACK guard PASS")
