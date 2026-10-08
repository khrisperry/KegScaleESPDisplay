#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
for test in emergency_touch control button foam_capture; do
  g++ -std=c++17 -Wall -Wextra -Werror "tests/${test}_test.cpp" -o "$test_dir/$test"
  "$test_dir/$test"
done
python3 tests/controller_test.py
python3 tests/pour_history_test.py
