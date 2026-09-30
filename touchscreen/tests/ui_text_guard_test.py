from pathlib import Path
from test_output import fail_exit, install_excepthook, pass_line

install_excepthook("Touch UI text guard")

root = Path(__file__).resolve().parents[1]
text_module = (root / "main" / "ui_text.cpp").read_text(encoding="utf-8")
text_header = (root / "main" / "ui_text.h").read_text(encoding="utf-8")
ui = (root / "main" / "ui.cpp").read_text(encoding="utf-8")
cmake = (root / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
wrapper = root / "main" / "ui_wrapper.cpp"

if wrapper.exists():
    fail_exit("Touch UI text guard: ui_wrapper.cpp must remain removed")
if '#include "ui.cpp"' in text_module or '#include "ui.cpp"' in ui:
    fail_exit("Touch UI text guard: Touch UI must not use source inclusion")
for macro in (
    "#define lv_label_set_text",
    "#define lv_label_set_text_fmt",
    "#define lv_textarea_set_text",
    "#define lv_dropdown_set_options",
):
    if macro in text_module or macro in ui:
        fail_exit(f"Touch UI text guard: Touch UI must not intercept LVGL with {macro}")

if '"ui.cpp" "ui_text.cpp"' not in cmake or "ui_wrapper.cpp" in cmake:
    fail_exit("Touch UI text guard: CMake must compile ui.cpp and ui_text.cpp directly")

required_calls = [
    "touchscreen_text::label_set_text(",
    "touchscreen_text::label_set_text_fmt(",
    "touchscreen_text::textarea_set_text(",
    "touchscreen_text::dropdown_set_options(",
]
for call in required_calls:
    if call not in ui:
        fail_exit(f"Touch UI text guard: missing explicit touchscreen text-safety call: {call}")

required_mappings = [
    'replacement = "-";',
    'replacement = "...";',
    'replacement = "\'";',
    'replacement = "\\\"";',
    'replacement = " deg";',
]
for mapping in required_mappings:
    if mapping not in text_module:
        fail_exit(f"Touch UI text guard: missing text-safety mapping: {mapping}")

for declaration in (
    "void label_set_text(lv_obj_t *object, const char *text);",
    "void label_set_text_fmt(lv_obj_t *object, const char *format, ...);",
    "void textarea_set_text(lv_obj_t *object, const char *text);",
    "void dropdown_set_options(lv_obj_t *object, const char *options);",
):
    if declaration not in text_header:
        fail_exit(f"Touch UI text guard: missing text API declaration: {declaration}")

for glyph in ("—", "…", "•"):
    if glyph not in ui:
        fail_exit(
            f"Touch UI text guard: legacy UI no longer contains {glyph!r}; update this regression test"
        )

# A locally flashed development build can legitimately be newer than the
# published feed. Keep downgrade protection, but do not present that as a
# feed synchronization failure to the user.
if 'case OtaView::Stale:\n    return "Local build newer";' not in ui:
    fail_exit("Touch UI text guard: newer-than-feed status must identify the local build")
if '"Feed syncing"' in ui:
    fail_exit("Touch UI text guard: misleading Feed syncing status must remain removed")

pass_line("Touch UI text guard")
