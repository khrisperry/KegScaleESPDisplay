#!/usr/bin/env bash
# Shared terminal output helpers for host-test runners.
# PASS is green and FAIL is red on an interactive terminal. Set NO_COLOR=1 to
# disable ANSI color while preserving machine-readable PASS:/FAIL: prefixes.

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  TEST_GREEN=$'\033[32m'
  TEST_RED=$'\033[31m'
  TEST_RESET=$'\033[0m'
else
  TEST_GREEN=''
  TEST_RED=''
  TEST_RESET=''
fi

pass_line() {
  printf '%sPASS:%s %s\n' "$TEST_GREEN" "$TEST_RESET" "$*"
}

fail_line() {
  printf '%sFAIL:%s %s\n' "$TEST_RED" "$TEST_RESET" "$*" >&2
}

run_test() {
  local label="$1"
  shift
  local output_file
  local status
  local line
  local emitted=0

  output_file="$(mktemp)"
  set +e
  "$@" >"$output_file" 2>&1
  status=$?
  set -e

  if (( status == 0 )); then
    while IFS= read -r line || [[ -n "$line" ]]; do
      [[ -z "$line" ]] && continue
      if [[ "$line" == PASS:* ]]; then
        pass_line "${line#PASS: }"
      else
        pass_line "$label: $line"
      fi
      emitted=1
    done <"$output_file"
    if (( emitted == 0 )); then
      pass_line "$label"
    fi
  else
    while IFS= read -r line || [[ -n "$line" ]]; do
      [[ -z "$line" ]] && continue
      if [[ "$line" == FAIL:* ]]; then
        fail_line "${line#FAIL: }"
      else
        fail_line "$label: $line"
      fi
      emitted=1
    done <"$output_file"
    if (( emitted == 0 )); then
      fail_line "$label"
    fi
  fi

  rm -f "$output_file"
  return "$status"
}
