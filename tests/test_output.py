#!/usr/bin/env python3
"""Consistent colored PASS:/FAIL: output for host regression scripts."""

from __future__ import annotations

import os
import subprocess
import sys
from typing import Iterable, Sequence

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
    """Render uncaught test errors as one FAIL line instead of a traceback."""

    def hook(exc_type, value, traceback) -> None:
        if issubclass(exc_type, KeyboardInterrupt):
            sys.__excepthook__(exc_type, value, traceback)
            return
        detail = str(value).strip() or exc_type.__name__
        detail = " | ".join(line.strip() for line in detail.splitlines() if line.strip())
        fail_line(f"{label}: {detail}")

    sys.excepthook = hook


def _emit_success_line(label: str, line: str) -> None:
    if line.startswith("PASS:"):
        pass_line(line[len("PASS:"):].strip())
    else:
        pass_line(f"{label}: {line}")


def _emit_failure_line(label: str, line: str) -> None:
    if line.startswith("FAIL:"):
        fail_line(line[len("FAIL:"):].strip())
    else:
        fail_line(f"{label}: {line}")


def run_command(label: str, command: Sequence[str]) -> None:
    """Run a child test/compile command and normalize every output line."""

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
                _emit_success_line(label, line)
        else:
            pass_line(label)
        return

    if lines:
        for line in lines:
            _emit_failure_line(label, line)
    else:
        fail_line(label)
    raise SystemExit(completed.returncode)
