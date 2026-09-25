#!/usr/bin/env python3
"""Guard the wrapper-free Touch application entrypoint."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = (root / "main" / "main.cpp").read_text(encoding="utf-8")
transport = (root / "main" / "connection_transport.cpp").read_text(encoding="utf-8")
transport_header = (root / "main" / "connection_transport.h").read_text(encoding="utf-8")
discovery = (root / "main" / "setup_discovery.cpp").read_text(encoding="utf-8")
setup_client = (root / "main" / "controller_setup_client.cpp").read_text(encoding="utf-8")
session = (root / "main" / "connection_session.cpp").read_text(encoding="utf-8")
pairing_guard = (root / "main" / "pairing_guard.cpp").read_text(encoding="utf-8")
cmake = (root / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
defaults = (root / "sdkconfig.defaults").read_text(encoding="utf-8")
wrapper = root / "main" / "main_wrapper.cpp"

checks = {
    "Touch builds main.cpp directly":
        'SRCS "main.cpp" "app_task.cpp"' in cmake and
        "main_wrapper.cpp" not in cmake,
    "main source-inclusion wrapper removed":
        not wrapper.exists(),
    "UI source-inclusion wrapper removed":
        not (root / "main" / "ui_wrapper.cpp").exists() and
        '"ui.cpp" "ui_text.cpp"' in cmake and
        '#include "ui.cpp"' not in (root / "main" / "ui.cpp").read_text(encoding="utf-8"),
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
    "WebSocket transport is isolated from application policy":
        '"connection_transport.cpp"' in cmake and
        '#include "connection_transport.h"' in main and
        'struct ScaleConnection' not in main and
        'struct Frame' not in main and
        'void socket_event(' not in main and
        'void transport_retirement_task(void *)' in transport and
        'retire_transport_async' in transport and
        'xQueueCreate(8, sizeof(RetiredTransport))' in transport and
        'frames = xQueueCreate(8, sizeof(Frame));' in transport and
        'connection_transport_init();' in main and
        'connection_receive_frame(&f, 0)' in main and
        'struct ScaleConnection' in transport_header and
        'struct Frame' in transport_header,
    "all Scale versions use authenticated application heartbeat":
        'current_time - c.last_ping > 8000000' in main and
        '!scale_supports_protocol_keepalive' not in main and
        'Scale %u heartbeat send failed; reconnect scheduled' in main,
    "WebSocket TX uses separate component lock":
        'CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y' in defaults and
        'CONFIG_ESP_WS_CLIENT_TX_LOCK_TIMEOUT_MS=2000' in defaults,
    "authenticated heartbeat owns connection liveness":
        "config.disable_pingpong_discon = true;" in main and
        'current_time - c.last_ping > 8000000' in main and
        'current_time - c.last_state > kStateStaleUs' in main,
    "Scale reconnect never stops its WebSocket synchronously":
        "esp_websocket_client_stop(c.ws)" not in main and
        "esp_websocket_client_destroy(c.ws)" not in main and
        "Queueing previous scale %u WebSocket for background retirement" in main and
        "esp_websocket_client_stop(retired.handle)" in transport and
        "esp_websocket_client_destroy(retired.handle)" in transport,
    "Session crypto and handshake are isolated":
        '"connection_session.cpp"' in cmake and
        '#include "connection_session.h"' in main and
        'cl_seal(' not in main and
        'cl_keypair(' not in main and
        'cl_agree(' not in main and
        'cl_start(' not in main and
        'esp_fill_random(' not in main and
        'connection_session_reset(slot)' in main and
        'connection_session_send_secure(slot, plain, now())' in main and
        'connection_session_prepare_hello(' in main and
        'connection_session_accept_handshake(' in main and
        'cl_seal(' in session and
        'cl_keypair(' in session and
        'cl_agree(' in session and
        'cl_start(' in session and
        'esp_fill_random(' in session,
    "Calibration ownership uses Scale-issued session IDs":
        "uint32_t calibration_session_id = 0;" in transport_header and
        "c.calibration_session_id = 0;" in session and
        'num(o, "calibration_session_id")' in main and
        'o, "calibration_session_id", c.calibration_session_id' in main and
        "Cancel calibration before switching scales." in main and
        "Calibration session is no longer active. Start calibration again." in main,
    "Controller setup HTTP is centralized":
        '"controller_setup_client.cpp"' in cmake and
        '#include "controller_setup_client.h"' in main and
        '#include "controller_setup_client.h"' in pairing_guard and
        '/api/controller' not in main and
        '/api/controller' not in pairing_guard and
        setup_client.count('/api/controller') == 2 and
        'controller_setup_get(host, 2000, &response)' in main and
        'controller_setup_remove(host, 3000, &status)' in main and
        'controller_setup_get(host, 1500, &response)' in pairing_guard and
        'controller_setup_remove(host, 2000, &http_status)' in pairing_guard,
    "Setup discovery never blocks the application task":
        '"setup_discovery.cpp"' in cmake and
        '#include "setup_discovery.h"' in main and
        'setup_discovery_start_wifi_scan(a.ui_generation)' in main and
        'setup_discovery_start_scale_scan(a.ui_generation)' in main and
        'xTaskCreate(task, task_name' in discovery and
        'esp_wifi_scan_start(&scan, true)' in discovery and
        'mdns_query_ptr("_kegscale", "_tcp", 3000, 8, &found)' in discovery,
    "UI actions get service during frame bursts":
        "processed < 8" in main and
        "give UI/actions a turn" in main,
    "retirement gate is set before work is queued":
        transport.find("c.retirement_pending = true;") >= 0 and
        transport.find("xQueueSend(retired_transports, &retired, 0)") >= 0 and
        transport.find("c.retirement_pending = true;") <
        transport.find("xQueueSend(retired_transports, &retired, 0)"),
}

failed = [name for name, passed in checks.items() if not passed]
if failed:
    raise SystemExit("Touch main architecture guard failed: " + "; ".join(failed))

print("Touch main architecture guard PASS")
