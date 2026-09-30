#!/usr/bin/env python3
"""Synchronize the Touch controller_link mirror from the companion Scale checkout."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

_USE_COLOR = sys.stderr.isatty() and not os.environ.get("NO_COLOR")
_RED = "\033[31m" if _USE_COLOR else ""
_RESET = "\033[0m" if _USE_COLOR else ""


def fail_line(message):
    print(f"{_RED}FAIL:{_RESET} {message}", file=sys.stderr)



def main():
    display = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scale-root",
        type=Path,
        default=Path(
            os.environ.get("KEGSCALE_SCALE_ROOT", display.parent / "KegScaleESP")
        ),
    )
    args = parser.parse_args()
    scale = args.scale_root.resolve()
    syncer = scale / "tools/sync_controller_link.py"
    if not syncer.is_file():
        fail_line(
            f"Scale controller_link sync tool missing: {syncer}; provide the companion "
            "Scale checkout with --scale-root or KEGSCALE_SCALE_ROOT"
        )
        return 1
    return subprocess.run(
        [
            sys.executable,
            str(syncer),
            "--write",
            "--scale-root",
            str(scale),
            "--display-root",
            str(display),
        ],
        check=False,
    ).returncode


if __name__ == "__main__":
    sys.exit(main())
