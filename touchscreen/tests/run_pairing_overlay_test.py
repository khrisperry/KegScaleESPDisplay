#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ota_test_helpers", root / "tests/run_ota_tests.py")
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)
with tempfile.TemporaryDirectory(prefix="keg-pairing-") as directory:
    tmp = Path(directory)
    (tmp / "pairing_overlay.inc").write_text(helpers.functions(root / "touchscreen/main/pairing_overlay.cpp", [
        "clear_pairing_timer_locked", "clear_pairing_overlay_locked", "request_pairing_cancel",
        "pairing_tick", "touchscreen_pairing_window", "touchscreen_pairing_ended"
    ]), encoding="utf-8")
    binary = tmp / "pairing-test"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(tmp),
                    str(root / "touchscreen/tests/pairing_overlay_test.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
