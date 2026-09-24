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
}

failed = [name for name, passed in checks.items() if not passed]
if failed:
    raise SystemExit("Touch main architecture guard failed: " + "; ".join(failed))

print("Touch main architecture guard PASS")
