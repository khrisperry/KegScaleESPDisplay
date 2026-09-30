#!/usr/bin/env python3
from pathlib import Path
from test_output import fail_exit, install_excepthook, pass_line

install_excepthook("Touch OTA signature wiring guard")

root = Path(__file__).resolve().parents[1]
ota = (root / "main/touchscreen_ota.cpp").read_text()
sig = (root / "main/ota_signature.c").read_text()
cmake = (root / "main/CMakeLists.txt").read_text()

assert '#include "ota_signature.h"' in ota
assert "manifest.sig?ref=main" in ota
assert '"ota_signature.c"' in cmake
assert "mbedtls" in cmake
assert "PSA_KEY_USAGE_VERIFY_MESSAGE" in sig
assert "psa_verify_message(" in sig
assert '"b22d59da8a84e17f"' in sig
assert '.channel = "beta"' in sig
assert '"16330b21be490f46"' in sig

verify_pos = ota.index("touchscreen_ota_signature_verify(")
parse_pos = ota.index("cJSON_Parse(manifest)")
assert verify_pos < parse_pos
assert "signature_result != ESP_OK" in ota
assert "return signature_result;" in ota

pass_line("signed Touch OTA manifest verification wiring guard")
