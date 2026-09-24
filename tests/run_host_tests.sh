#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$root/tools/check_controller_link.py"
python3 "$root/tests/display_command_ack_guard_test.py"
test_binary="$(mktemp)"
trap 'rm -f "$test_binary"' EXIT
cc -std=c11 -Wall -Wextra -Werror "$root/tests/power_policy_test.c" -o "$test_binary"
"$test_binary"
cc -std=c11 -Wall -Wextra -Werror -I"$root/tests/touch_stubs" \
  -I"$root/components/touch_wake/include" "$root/tests/touch_calibration_test.c" -o "$test_binary"
"$test_binary"
python3 "$root/tests/run_ota_tests.py"
python3 "$root/tests/run_wake_tests.py"
python3 "$root/touchscreen/tests/run_pairing_overlay_test.py"
bash "$root/touchscreen/tests/run_host_tests.sh"
