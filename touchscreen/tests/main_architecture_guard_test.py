#!/usr/bin/env python3
"""Guard the wrapper-free Touch application entrypoint."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = (root / "main" / "main.cpp").read_text(encoding="utf-8")
cmake = (root / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
wrapper = root / "main" / "main_wrapper.cpp"

checks = {
    "Touch builds main.cpp directly":
        'SRCS "main.cpp" "app_task.cpp"' in cmake and
        "main_wrapper.cpp" not in cmake,
    "main source-inclusion wrapper removed":
        not wrapper.exists(),
    "application entrypoint is explicit":
        'extern "C" void touchscreen_app_main()' in main,
    "settings apply is explicit":
        "touchscreen_apply_settings_live();" in main and
        "#define esp_restart" not in main,
    "UI message interception is explicit":
        "static void touchscreen_ui_message(const char *message);" in main and
        "static void touchscreen_ui_message(const char *message) {" in main and
        "touchscreen_touchscreen_ui_message" not in main and
        "#define ui_message" not in main,
    "pairing key cleanup is explicit":
        "touchscreen_pairing_memset(scale_master(slot), 0, 32);" in main and
        "#define memset" not in main,
    "encrypted unpair handling is explicit":
        "touchscreen_pairing_cl_open(&c.link, f.bytes, f.length, plain);" in main and
        "#define cl_open" not in main,
    "legacy OTA action uses scheduler":
        "touchscreen_ota_request(false, true)" in main,
    "WebSocket teardown is isolated from the main task":
        'void transport_retirement_task(void *)' in main and
        'retire_transport_async' in main and
        'retirement_pending' in main and
        'xQueueCreate(8, sizeof(RetiredTransport))' in main and
        'frames = xQueueCreate(16, sizeof(Frame));' in main,
    "heartbeat send cannot hold the app loop for one second":
        'esp_websocket_client_send_bin(c.ws, (char *)out, len,' in main and
        'pdMS_TO_TICKS(50)' in main,
    "Scale reconnect never stops its WebSocket synchronously":
        "esp_websocket_client_stop(c.ws)" not in main and
        "esp_websocket_client_destroy(c.ws)" not in main and
        "Queueing previous scale %u WebSocket for background retirement" in main,
    "UI actions get service during frame bursts":
        "processed < 8" in main and
        "give UI/actions a turn" in main,
    "retirement gate is set before work is queued":
        main.find("c.retirement_pending = true;") <
        main.find("xQueueSend(retired_transports, &retired, 0)"),
}

failed = [name for name, passed in checks.items() if not passed]
if failed:
    raise SystemExit("Touch main architecture guard failed: " + "; ".join(failed))

print("Touch main architecture guard PASS")
