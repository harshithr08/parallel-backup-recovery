"""Shared helpers for the IPBRS benchmark scripts (standard library only)."""

from __future__ import annotations

import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "build" / ("ipbrs.exe" if os.name == "nt" else "ipbrs")
DEFAULT_DATA = REPO / "data"


class EngineError(RuntimeError):
    pass


def engine() -> Path:
    if not ENGINE.is_file():
        sys.exit(f"engine not found at {ENGINE}; build it first (build.bat)")
    return ENGINE


def run_engine(*args: str, allow_status: tuple[int, ...] = (0,)) -> dict:
    """Runs one engine command and returns its JSON result record."""
    completed = subprocess.run([str(engine()), *args], capture_output=True, text=True)
    if completed.returncode not in allow_status:
        raise EngineError(f"ipbrs {' '.join(args)} exited {completed.returncode}: {completed.stderr.strip()}")
    lines = [line for line in completed.stdout.splitlines() if line.startswith("{")]
    if not lines:
        raise EngineError(f"ipbrs {' '.join(args)} printed no result: {completed.stdout!r}")
    return json.loads(lines[-1])


def remove_tree(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)


def free_bytes(path: Path) -> int:
    probe = path
    while not probe.exists():
        probe = probe.parent
    return shutil.disk_usage(probe).free


def environment() -> dict:
    """Machine description stored with every result file."""
    return {
        "platform": platform.platform(),
        "processor": platform.processor(),
        "logical_cpus": os.cpu_count(),
        "python": platform.python_version(),
    }
