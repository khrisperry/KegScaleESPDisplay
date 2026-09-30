#!/usr/bin/env python3
"""Consistent colored PASS:/FAIL: output for Touch host regression scripts."""

from __future__ import annotations

import os
import subprocess
import sys
from typing import Sequence

_USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
_GREEN = "\033[32m" if _USE_COLOR else ""
_RED = "\033[31m" if _USE_COLOR else ""
_RESET = "\033[0m" if _USE_COLOR else ""


def pass_line(message: str) -> None:
    print(f"{_GREEN}PASS:{_RESET} {message}")


def fail_line(message: str) -> None:
    print(f"{_RED}FAIL:{_RESET} {message}", file=sys.stderr)


def fail_exit(message: str, code: int = 1) -> None:
    fail_line(message)
    raise SystemExit(code)


def install_excepthook(label: str) -> None:
    def hook(exc_type, value, traceback) -> None:
        if issubclass(exc_type, KeyboardInterrupt):
            sys.__excepthook__(exc_type, value, traceback)
            return
        detail = str(value).strip() or exc_type.__name__
        detail = " | ".join(line.strip() for line in detail.splitlines() if line.strip())
        fail_line(f"{label}: {detail}")

    sys.excepthook = hook


def run_command(label: str, command: Sequence[str]) -> None:
    completed = subprocess.run(
        list(command),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if completed.returncode == 0:
        if lines:
            for line in lines:
                if line.startswith("PASS:"):
                    pass_line(line[len("PASS:"):].strip())
                else:
                    pass_line(f"{label}: {line}")
        else:
            pass_line(label)
        return

    if lines:
        for line in lines:
            if line.startswith("FAIL:"):
                fail_line(line[len("FAIL:"):].strip())
            else:
                fail_line(f"{label}: {line}")
    else:
        fail_line(label)
    raise SystemExit(completed.returncode)
