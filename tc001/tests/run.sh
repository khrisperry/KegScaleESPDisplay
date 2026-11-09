#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_bin="$(mktemp)"
trap 'rm -f "$test_bin"' EXIT
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main main/pixel_ui.c tests/test_pixel_ui.c -lm -o "$test_bin"
"$test_bin" "preview/frames.js"
# Use the managed component's sources, an explicit cJSON checkout, or libcjson-dev.
if [[ -n "${CJSON_DIR:-}" ]]; then
    json_flags=(-I "$CJSON_DIR" "$CJSON_DIR/cJSON.c")
elif [[ -f managed_components/espressif__cjson/cJSON.c ]]; then
    json_flags=(-I managed_components/espressif__cjson managed_components/espressif__cjson/cJSON.c)
else
    read -ra json_flags <<< "$(pkg-config --cflags --libs libcjson)"
fi
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main main/mqtt_feed.c tests/test_mqtt_feed.c "${json_flags[@]}" -lm -o "$test_bin"
"$test_bin"
node tests/test_setup.js
