#!/usr/bin/env python3
"""Docker healthcheck: verify owned children, collector socket and fresh rawlog."""
import json
import os
from pathlib import Path
import socket
import stat
import sys
import time


def proc_starttime(pid: int) -> int:
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    return int(fields[19])


def healthy(state_file: Path, socket_path: Path) -> bool:
    state = json.loads(state_file.read_text())
    for name in ("netatop", "atop"):
        pid = int(state[f"{name}_pid"])
        if proc_starttime(pid) != int(state[f"{name}_starttime"]):
            return False
    if not stat.S_ISSOCK(socket_path.stat().st_mode):
        return False
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(1)
        connection.connect(str(socket_path))
    rawfile = Path(state["active_file"])
    info = rawfile.stat()
    interval = int(state["interval_seconds"])
    return (
        interval > 0
        and info.st_size > 0
        and time.time() - info.st_mtime <= max(30, 3 * interval)
    )


if __name__ == "__main__":
    try:
        ok = healthy(
            Path(os.environ.get("ATOP_STATE_FILE", "/run/collector-state.json")),
            Path(os.environ.get("NETATOP_SOCKET", "/var/run/netatop-bpf-socket")),
        )
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError):
        ok = False
    sys.exit(0 if ok else 1)
