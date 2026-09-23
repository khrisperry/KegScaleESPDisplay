#!/usr/bin/env bash
set -euo pipefail
# Requires libmbedtls-dev, or explicit MBEDTLS_INCLUDE and MBEDTLS_LIBRARY.
root="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$root/tests/ui_text_guard_test.py"
python3 "$root/tests/ui_lifetime_guard_test.py"
python3 "$root/tests/run_connection_test.py"
test_binary="$(mktemp)"
lifetime_binary="$(mktemp)"
trap 'rm -f "$test_binary" "$lifetime_binary"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror \
  -I"$root/main" "$root/tests/ui_lifetime_test.cpp" -o "$lifetime_binary"
"$lifetime_binary"
cc -std=c11 -Wall -Wextra -Werror \
  -I"${MBEDTLS_INCLUDE:-/usr/include}" -I"$root/tests/host" \
  -I"$root/components/controller_link/include" \
  "$root/components/controller_link/controller_link.c" \
  "$root/tests/controller_link_test.c" \
  "${MBEDTLS_LIBRARY:--lmbedcrypto}" -o "$test_binary"
"$test_binary"
cc -std=c11 -Wall -Wextra -Werror \
  -I"${MBEDTLS_INCLUDE:-/usr/include}" -I"$root/tests/host" \
  -I"$root/components/controller_link/include" \
  "$root/components/controller_link/controller_link.c" \
  "$root/tests/controller_link_failure_test.c" \
  -Wl,--wrap=psa_destroy_key,--wrap=psa_export_public_key,--wrap=psa_raw_key_agreement,--wrap=psa_import_key,--wrap=psa_aead_encrypt,--wrap=psa_aead_decrypt \
  "${MBEDTLS_LIBRARY:--lmbedcrypto}" -o "$test_binary"
"$test_binary"
