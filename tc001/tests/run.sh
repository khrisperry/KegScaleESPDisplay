#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_bin="$(mktemp)"
trap 'rm -f "$test_bin"' EXIT
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main main/pixel_ui.c tests/test_pixel_ui.c -lm -o "$test_bin"
"$test_bin" "preview/frames.js"
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main tests/test_wifi_policy.c -o "$test_bin"
"$test_bin"
