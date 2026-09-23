#!/usr/bin/env python3
"""Run the canonical consistency checker from the companion Scale checkout."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    display = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scale-root", type=Path, default=Path(os.environ.get(
        "KEGSCALE_SCALE_ROOT", display.parent / "KegScaleESP")))
    args = parser.parse_args()
    scale = args.scale_root.resolve()
    checker = scale / "tools/check_controller_link.py"
    if not checker.is_file():
        print(f"FAIL: Scale consistency checker missing: {checker}\n"
              "Provide the companion Scale checkout with --scale-root or KEGSCALE_SCALE_ROOT.", file=sys.stderr)
        return 1
    return subprocess.run([sys.executable, str(checker), "--scale-root", str(scale),
                           "--display-root", str(display)], check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
