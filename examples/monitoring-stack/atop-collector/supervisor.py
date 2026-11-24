#!/usr/bin/env python3
"""Run the kernel-compatible netatop collector and atop raw logger together."""
import datetime as dt
import json
import os
from pathlib import Path
import re
import signal
import socket
import stat
import subprocess
import sys
import time


LOG_NAME = re.compile(r"atop_[0-9]{8}_[0-9]+\Z")


def positive_int(name: str, default: int) -> int:
    value = int(os.environ.get(name, str(default)))
    if value <= 0:
        raise ValueError(f"{name} must be positive")
    return value


def proc_starttime(pid: int) -> int:
    # /proc/PID/stat's comm field may contain spaces or parentheses.
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    return int(fields[19])  # field 22, after pid and comm


def utc_now() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


def wait_socket(path: Path, child: subprocess.Popen, timeout: float = 15) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise RuntimeError(f"netatop exited with {child.returncode} before socket startup")
        try:
            if stat.S_ISSOCK(path.stat().st_mode):
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(1)
                    connection.connect(str(path))
                return
        except (FileNotFoundError, ConnectionRefusedError, OSError):
            pass
        time.sleep(0.1)
    raise RuntimeError(f"netatop socket {path} did not become ready")


def write_state(path: Path, netatop: subprocess.Popen, atop: subprocess.Popen,
                rawfile: Path, interval: int) -> None:
    state = {
        "netatop_pid": netatop.pid,
        "netatop_starttime": proc_starttime(netatop.pid),
        "atop_pid": atop.pid,
        "atop_starttime": proc_starttime(atop.pid),
        "active_file": str(rawfile),
        "interval_seconds": interval,
    }
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state) + "\n")
    os.replace(temporary, path)


def prune_logs(directory: Path, current: Path, retention_days: int,
               now: dt.datetime) -> None:
    cutoff = now.timestamp() - retention_days * 86400
    for candidate in directory.iterdir():
        if candidate == current or not LOG_NAME.fullmatch(candidate.name):
            continue
        try:
            info = candidate.lstat()
            if stat.S_ISREG(info.st_mode) and info.st_mtime < cutoff:
                candidate.unlink()
        except FileNotFoundError:
            pass


def stop_child(child: subprocess.Popen | None, gentle_signal: int,
               timeout: float = 5) -> None:
    if child is None or child.poll() is not None:
        return
    child.send_signal(gentle_signal)
    try:
        child.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait(timeout=timeout)


def run() -> int:
    interval = positive_int("ATOP_INTERVAL_SECONDS", 10)
    retention = positive_int("ATOP_RETENTION_DAYS", 7)
    log_dir = Path(os.environ.get("ATOP_LOG_DIR", "/var/log/atop"))
    state_file = Path(os.environ.get("ATOP_STATE_FILE", "/run/collector-state.json"))
    socket_path = Path(os.environ.get("NETATOP_SOCKET", "/var/run/netatop-bpf-socket"))
    netatop_bin = os.environ.get("NETATOP_BIN", "/usr/local/bin/netatop")
    atop_bin = os.environ.get("ATOP_BIN", "/usr/local/bin/atop")
    os.umask(0o022)
    log_dir.mkdir(parents=True, exist_ok=True)
    stopping = False

    def on_stop(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, on_stop)
    signal.signal(signal.SIGINT, on_stop)
    netatop = None
    atop = None
    try:
        netatop = subprocess.Popen([netatop_bin])
        wait_socket(socket_path, netatop)
        day = ""
        rawfile = None
        last_observed = None
        last_progress = time.monotonic()
        while not stopping:
            if netatop.poll() is not None:
                raise RuntimeError(f"netatop exited with {netatop.returncode}")
            now = utc_now()
            today = now.strftime("%Y%m%d")
            if today != day:
                stop_child(atop, signal.SIGUSR2)
                rawfile = log_dir / f"atop_{today}_{time.time_ns()}"
                atop = subprocess.Popen([atop_bin, "-K", "-w", str(rawfile), str(interval)])
                day = today
                write_state(state_file, netatop, atop, rawfile, interval)
                prune_logs(log_dir, rawfile, retention, now)
                last_observed = None
                last_progress = time.monotonic()
            if atop.poll() is not None:
                raise RuntimeError(f"atop exited with {atop.returncode}")
            try:
                info = rawfile.stat()
                observed = (info.st_size, info.st_mtime_ns)
                if info.st_size > 0 and observed != last_observed:
                    last_observed = observed
                    last_progress = time.monotonic()
            except FileNotFoundError:
                pass
            if time.monotonic() - last_progress > max(30, 3 * interval):
                raise RuntimeError(f"atop rawlog stopped advancing: {rawfile}")
            time.sleep(1)
        return 0
    finally:
        state_file.unlink(missing_ok=True)
        stop_child(atop, signal.SIGUSR2)
        stop_child(netatop, signal.SIGTERM)


if __name__ == "__main__":
    try:
        sys.exit(run())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"atop collector: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
