#!/usr/bin/env bash
set -euo pipefail
# Requires libmbedtls-dev, or explicit MBEDTLS_INCLUDE and MBEDTLS_LIBRARY.
root="$(cd "$(dirname "$0")/.." && pwd)"
test_binary="$(mktemp)"
trap 'rm -f "$test_binary"' EXIT
cc -std=c11 -Wall -Wextra -Werror \
  -I"${MBEDTLS_INCLUDE:-/usr/include}" -I"$root/tests/host" \
  -I"$root/components/controller_link/include" \
  "$root/components/controller_link/controller_link.c" \
  "$root/tests/controller_link_test.c" \
  "${MBEDTLS_LIBRARY:--lmbedcrypto}" -o "$test_binary"
"$test_binary"
