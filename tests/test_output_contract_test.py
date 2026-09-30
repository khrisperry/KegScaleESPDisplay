#!/usr/bin/env python3
"""Guard the project-wide PASS:/FAIL: test output contract."""

from pathlib import Path
import re

from test_output import fail_exit, install_excepthook, pass_line

install_excepthook("Display/Touch test output contract")

ROOT = Path(__file__).resolve().parents[1]
failures = []

for directory in (ROOT / "tests", ROOT / "touchscreen/tests"):
    for path in sorted(directory.glob("*.py")):
        if path.name in {"test_output.py", "test_output_contract_test.py"}:
            continue
        source = path.read_text(encoding="utf-8")
        if "test_output" not in source:
            failures.append(f"{path.relative_to(ROOT)} does not use test_output.py")

display_runner = (ROOT / "tests/run_host_tests.sh").read_text(encoding="utf-8")
touch_runner = (ROOT / "touchscreen/tests/run_host_tests.sh").read_text(encoding="utf-8")
if 'source "$root/tests/test_output.sh"' not in display_runner:
    failures.append("tests/run_host_tests.sh does not source test_output.sh")
if 'source "$repo_root/tests/test_output.sh"' not in touch_runner:
    failures.append("touchscreen/tests/run_host_tests.sh does not source shared test_output.sh")
if "run_test " not in display_runner or "pass_line " not in display_runner:
    failures.append("Display host runner does not normalize test results")
if "run_test " not in touch_runner or "pass_line " not in touch_runner:
    failures.append("Touch host runner does not normalize test results")

for directory in (ROOT / "tests", ROOT / "touchscreen/tests"):
    for path in sorted([*directory.glob("*.c"), *directory.glob("*.cpp")]):
        source = path.read_text(encoding="utf-8")
        for message in re.findall(r'\b(?:puts|std::puts)\("([^"]*)"\)', source):
            if not message.startswith("PASS:"):
                failures.append(
                    f"{path.relative_to(ROOT)} has noncanonical result output: {message!r}"
                )

if failures:
    fail_exit("Display/Touch test output contract: " + "; ".join(failures))

pass_line("Display/Touch test output contract")
