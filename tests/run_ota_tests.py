#!/usr/bin/env python3
"""Compile production OTA functions with host fakes; no generated source is tracked."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def functions(path, names):
    source = path.read_text(encoding="utf-8")
    # Mask strings/comments before balancing braces, preserving source offsets.
    masked = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',
                    lambda match: " " * len(match[0]), source, flags=re.S)
    result = []
    for name in names:
        matches = list(re.finditer(r"^(?:static[ \t]+)?[A-Za-z_][\w* ]*[ \t*]" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", masked, re.M))
        if len(matches) != 1:
            raise RuntimeError(f"Expected one production definition of {name}, found {len(matches)}")
        start = matches[0].start()
        end = matches[0].end()
        depth = 1
        while depth:
            if masked[end] == "{": depth += 1
            if masked[end] == "}": depth -= 1
            end += 1
        result.append(source[start:end])
    return "\n\n".join(result)


def main():
    with tempfile.TemporaryDirectory(prefix="keg-ota-") as directory:
        tmp = Path(directory)
        (tmp / "ota_flow.inc").write_text(functions(ROOT / "main/main.c", [
            "display_update_needed", "install_display_update_if_needed"]), encoding="utf-8")
        (tmp / "ota_download.inc").write_text(functions(ROOT / "components/display_ota/display_ota.c", [
            "secure_zero", "sha256_matches", "download_and_stage_once"]), encoding="utf-8")
        for test in ["ota_authorization_policy_test", "ota_flow_test", "ota_download_test"]:
            binary = tmp / test
            command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-I" + str(tmp), "-I" + str(ROOT / "tests/ota_host"),
                       "-I" + str(ROOT / "components/ble_client/include"),
                       "-I" + str(ROOT / "components/display_ota/include"),
                       "-I" + str(ROOT / "main"), str(ROOT / f"tests/{test}.c"),
                       "-lmbedcrypto", "-o", str(binary)]
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
