#!/usr/bin/env python3
"""Ensure Waveshare 4.3-inch connection image cannot consume square 4B OTA feed."""
from pathlib import Path
root=Path(__file__).resolve().parents[2]
project=root/'touchscreen43'
source=(project/'main/app_task.cpp').read_text()
ota=(project/'main/ota_disabled.cpp').read_text()
cmake=(project/'CMakeLists.txt').read_text()
shared=(root/'touchscreen/main/main.cpp').read_text()
workflow=(root/'.github/workflows/touchscreen43-test.yml').read_text()
checks={
    '4.3-inch has separate firmware project': 'project(keg_scale_touchscreen43)' in cmake,
    'device has 4.3-inch compile configuration': 'KEG_TOUCHSCREEN_43=1' in (project/'main/CMakeLists.txt').read_text(),
    'automatic OTA scheduler never started': 'touchscreen_start_ota_scheduler();' not in source,
    '4B update handler explicitly disabled': 'ESP_ERR_NOT_SUPPORTED' in ota,
    'manual OTA request still routes through disabled update handler': 'touchscreen_ota(' in shared,
    'isolated 4.3 CI branch': 'test/waveshare-43-v1.4.24-connection' in workflow,
    'manual merged firmware built': 'merge-bin -o firmware-usb.bin @flash_args' in workflow,
    'no firmware-repo publish': 'KegScaleFirmware' not in workflow and 'git push' not in workflow,
    'scale crypto kept in shared source': 'connection_session_accept_handshake(' in shared,
    'updated 6-second heartbeat': 'current_time - c.last_ping > 6000000' in shared,
    'diagnostic firmware version': (project/'version.txt').read_text().strip()=='V1.4.25',
}
failed=[name for name,ok in checks.items() if not ok]
if failed: raise SystemExit('FAIL: Waveshare43 manual-only isolation: '+'; '.join(failed))
print('PASS: Waveshare43 V1.4.25 USB-only OTA isolation and connection policy')
