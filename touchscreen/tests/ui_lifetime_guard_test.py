#!/usr/bin/env python3
"""Guard the production Touch UI lifetime wiring used by async Setup work."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
ui = (root / "main/ui.cpp").read_text(encoding="utf-8")
main = (root / "main/main.cpp").read_text(encoding="utf-8")
discovery = (root / "main/setup_discovery.cpp").read_text(encoding="utf-8")
app = (root / "main/app.h").read_text(encoding="utf-8")

checks = {
    "Action carries UI generation":
        "uint32_t ui_generation;" in app,
    "UI actions capture current generation":
        "a.ui_generation = touchscreen_ui_generation_current(&content_generation);" in ui,
    "screen rebuild invalidates generation":
        ui.count("touchscreen_ui_generation_advance(&content_generation);") >= 2,
    "mDNS results preserve generation through worker":
        "setup_discovery_start_scale_scan(a.ui_generation)" in main and
        "ui_discovered_options_for_generation(options, generation);" in discovery,
    "Wi-Fi results preserve generation through worker":
        "setup_discovery_start_wifi_scan(a.ui_generation)" in main and
        "ui_networks(options[0] ? options : \"No networks found\", generation);" in discovery,
    "late async messages preserve generation":
        discovery.count("ui_message_for_generation(") >= 4 and
        main.count("ui_message_for_generation(") >= 2,
    "discovery callback rejects stale generation":
        "touchscreen_ui_generation_accepts(&content_generation, generation)" in ui and
        "Ignoring stale scale discovery result" in ui,
    "Wi-Fi callback rejects stale generation":
        "Ignoring stale Wi-Fi scan result" in ui,
}

failed = [name for name, passed in checks.items() if not passed]
if failed:
    raise SystemExit("Touch UI lifetime guard failed: " + "; ".join(failed))

print("Touch UI lifetime guard PASS")
