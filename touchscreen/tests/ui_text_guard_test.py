from pathlib import Path

root = Path(__file__).resolve().parents[1]
wrapper = (root / "main" / "ui_wrapper.cpp").read_text(encoding="utf-8")
legacy = (root / "main" / "ui.cpp").read_text(encoding="utf-8")

required_redirects = [
    "#define lv_label_set_text touchscreen_text::label_set_text",
    "#define lv_label_set_text_fmt touchscreen_text::label_set_text_fmt",
    "#define lv_textarea_set_text touchscreen_text::textarea_set_text",
    "#define lv_dropdown_set_options touchscreen_text::dropdown_set_options",
]
for redirect in required_redirects:
    if redirect not in wrapper:
        raise SystemExit(f"missing touchscreen text-safety redirect: {redirect}")

required_mappings = [
    'replacement = "-";',
    'replacement = "...";',
    'replacement = "\'";',
    'replacement = "\\\"";',
    'replacement = " deg";',
]
for mapping in required_mappings:
    if mapping not in wrapper:
        raise SystemExit(f"missing touchscreen text-safety mapping: {mapping}")

# These known legacy characters are intentionally kept as test fixtures: the
# rendering layer must remain capable of converting them before they reach the
# built-in LVGL Montserrat fonts.
for glyph in ("—", "…", "•"):
    if glyph not in legacy:
        raise SystemExit(
            f"legacy UI no longer contains {glyph!r}; update this regression test"
        )

print("touchscreen UI text guard: OK")
