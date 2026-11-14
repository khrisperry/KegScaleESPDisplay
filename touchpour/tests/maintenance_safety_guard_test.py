#!/usr/bin/env python3
"""Safety gate for supervised TouchPour maintenance images.

TouchPour may never be published through the 4-inch touchscreen Dev feed.
The actual servo and LiDAR behavior must still be checked locally with
the mechanical linkage disconnected before reconnecting the tap.
"""
from pathlib import Path

root=Path(__file__).resolve().parents[1]
app=(root/"components/tap_control/app.cpp").read_text()
network=(root/"components/tap_control/network.cpp").read_text()
display=(root/"../touchscreen/main/main.cpp").read_text()
cmake=(root/"CMakeLists.txt").read_text()
workflow=(root/"../.github/workflows/touchpour.yml").read_text()
checks={
    "independent TouchPour project": 'project(touchpour_display)' in cmake and
        'set(PROJECT_VER "0.1.23")' in cmake,
    "GPIO18 servo calibration enforced": "CONFIG_TAP_SERVO == 18" in app,
    "PWM initial duty calibrated closed only": "c.duty = settings.servo_calibrated" in app and
        "settings.closed_us * 16384 / 20000" in app,
    "boot closes calibrated servo": "set_servo(settings.closed_us)" in app,
    "upload refuses active pour": "pouring_active() || manual_test_active || firmware_updating" in network,
    "upload latches maintenance before write": "firmware_updating = true;" in network and
        "update_closed = false;" in network,
    "upload waits for servo close confirmation": "if (!closed) return fail(" in network and
        "update_closed = servo_us == settings.closed_us;" in app,
    "interrupted upload stays fail-closed until reboot": "firmware_updating = true;" in network and
        "Firmware update failed: restart required" in network,
    "new image identity is TouchPour only": "esp_app_get_description()->project_name" in network and
        "TouchPour ESP32-S3 application .bin" in network,
    "tap start blocked during maintenance": "if (firmware_updating)" in app and
        "control.stop(\"Firmware update: tap closed\")" in app,
    "emergency touch retains precedence": "touch_stop_armed" in app and
        "stop_requested.exchange(false)" in app,
    "WebSocket client retains crypto": "connection_session_prepare_hello" in display and
        "connection_session_accept_handshake" in display,
    "no TouchPour OTA auto-feed publishing": "actions/upload-artifact@v4" in workflow and
        "KegScaleFirmware" not in workflow and
        "git push" not in workflow,
}
failed=[x for x,v in checks.items() if not v]
if failed: raise SystemExit("FAIL TouchPour manual maintenance safety: "+", ".join(failed))
print("PASS TouchPour V0.1.23 fail-closed manual maintenance safety guard")
