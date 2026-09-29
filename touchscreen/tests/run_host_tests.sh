#!/usr/bin/env bash
set -euo pipefail
# Requires libmbedtls-dev, or explicit MBEDTLS_INCLUDE and MBEDTLS_LIBRARY.
# Repository .gitattributes keeps this script LF-only for Windows + WSL checkouts.
root="$(cd "$(dirname "$0")/.." && pwd)"
repo_root="$(cd "$root/.." && pwd)"
# shellcheck source=../../tests/test_output.sh
source "$repo_root/tests/test_output.sh"

run_test "Touch UI text guard" python3 "$root/tests/ui_text_guard_test.py"
run_test "Touch UI lifetime guard" python3 "$root/tests/ui_lifetime_guard_test.py"
run_test "Touch main architecture guard" python3 "$root/tests/main_architecture_guard_test.py"
run_test "Touch OTA signature wiring guard" python3 "$root/tests/ota_signature_guard_test.py"
run_test "Touch pairing rearm isolation guard" python3 "$root/tests/pairing_rearm_isolation_guard_test.py"
run_test "Touch connection regression" python3 "$root/tests/run_connection_test.py"

test_binary="$(mktemp)"
lifetime_binary="$(mktemp)"
trap 'rm -f "$test_binary" "$lifetime_binary"' EXIT

run_test "Touch UI lifetime test compile" c++ -std=c++17 -Wall -Wextra -Werror   -I"$root/main" "$root/tests/ui_lifetime_test.cpp" -o "$lifetime_binary"
run_test "Touch UI lifetime test" "$lifetime_binary"

run_test "Touch controller_link crypto compile" cc -std=c11 -Wall -Wextra -Werror   -I"${MBEDTLS_INCLUDE:-/usr/include}" -I"$root/tests/host"   -I"$root/components/controller_link/include"   "$root/components/controller_link/controller_link.c"   "$root/tests/controller_link_test.c"   "${MBEDTLS_LIBRARY:--lmbedcrypto}" -o "$test_binary"
run_test "Touch controller_link crypto" "$test_binary"

run_test "Touch OTA signature crypto compile" cc -std=c11 -Wall -Wextra -Werror   -I"${MBEDTLS_INCLUDE:-/usr/include}" -I"$root/tests/host" -I"$root/main"   "$root/main/ota_signature.c"   "$root/tests/ota_signature_crypto_test.c"   "${MBEDTLS_LIBRARY:--lmbedcrypto}" -o "$test_binary"
run_test "Touch OTA signature crypto" "$test_binary"

run_test "Touch controller_link failure compile" cc -std=c11 -Wall -Wextra -Werror   -I"${MBEDTLS_INCLUDE:-/usr/include}" -I"$root/tests/host"   -I"$root/components/controller_link/include"   "$root/components/controller_link/controller_link.c"   "$root/tests/controller_link_failure_test.c"   -Wl,--wrap=psa_destroy_key,--wrap=psa_export_public_key,--wrap=psa_raw_key_agreement,--wrap=psa_import_key,--wrap=psa_aead_encrypt,--wrap=psa_aead_decrypt   "${MBEDTLS_LIBRARY:--lmbedcrypto}" -o "$test_binary"
run_test "Touch controller_link failure injection" "$test_binary"

pass_line "Touch host suite complete"
