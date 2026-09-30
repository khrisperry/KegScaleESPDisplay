#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=test_output.sh
source "$root/tests/test_output.sh"

run_test "Display/Touch test output contract" python3 "$root/tests/test_output_contract_test.py"
run_test "controller_link canonical mirror check" python3 "$root/tools/check_controller_link.py"
run_test "display command ACK guard" python3 "$root/tests/display_command_ack_guard_test.py"

test_binary="$(mktemp)"
trap 'rm -f "$test_binary"' EXIT

run_test "e-paper power policy compile" cc -std=c11 -Wall -Wextra -Werror   "$root/tests/power_policy_test.c" -o "$test_binary"
run_test "e-paper power policy" "$test_binary"

run_test "touch calibration compile" cc -std=c11 -Wall -Wextra -Werror   -I"$root/tests/touch_stubs" -I"$root/components/touch_wake/include"   "$root/tests/touch_calibration_test.c" -o "$test_binary"
run_test "touch calibration" "$test_binary"

run_test "e-paper OTA regression suite" python3 "$root/tests/run_ota_tests.py"
run_test "e-paper wake regression suite" python3 "$root/tests/run_wake_tests.py"

# The Touch runner sources the same output helper. Run it directly so individual
# PASS lines remain PASS even if a later Touch test fails.
bash "$root/touchscreen/tests/run_host_tests.sh"

pass_line "Display/e-paper/Touch host suite complete"
