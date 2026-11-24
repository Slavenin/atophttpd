#!/usr/bin/env python3
"""Lifecycle checks use local stand-ins; no BPF access or Docker is required."""
import datetime as dt
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import health  # noqa: E402
import supervisor  # noqa: E402


NETATOP_STANDIN = '''#!/usr/bin/env python3
import os, signal, socket, time
from pathlib import Path
sock = Path(os.environ["NETATOP_SOCKET"])
server = socket.socket(socket.AF_UNIX)
server.bind(str(sock))
server.listen()
server.settimeout(.2)
stopping = False
def stop(*_):
    global stopping
    stopping = True
signal.signal(signal.SIGTERM, stop)
while not stopping:
    try:
        client, _ = server.accept()
        client.close()
    except socket.timeout:
        pass
server.close()
sock.unlink(missing_ok=True)
'''

ATOP_STANDIN = '''#!/usr/bin/env python3
import signal, sys, time
from pathlib import Path
assert sys.argv[1:3] == ["-K", "-w"]
path = Path(sys.argv[3])
assert int(sys.argv[4]) > 0
stopping = False
def stop(*_):
    global stopping
    stopping = True
signal.signal(signal.SIGUSR2, stop)
with path.open("ab", buffering=0) as stream:
    while not stopping:
        stream.write(b"sample")
        time.sleep(.2)
'''


class SupervisorTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.logs = self.root / "logs"
        self.state = self.root / "state.json"
        self.sock = self.root / "netatop.sock"
        self.netatop = self.root / "netatop"
        self.atop = self.root / "atop"
        for path, body in ((self.netatop, NETATOP_STANDIN), (self.atop, ATOP_STANDIN)):
            path.write_text(body)
            path.chmod(0o755)
        self.env = dict(os.environ, ATOP_LOG_DIR=str(self.logs),
                        ATOP_STATE_FILE=str(self.state), NETATOP_SOCKET=str(self.sock),
                        NETATOP_BIN=str(self.netatop), ATOP_BIN=str(self.atop),
                        ATOP_INTERVAL_SECONDS="1", ATOP_RETENTION_DAYS="7")

    def start(self):
        process = subprocess.Popen([sys.executable, str(HERE / "supervisor.py")],
                                   env=self.env, stderr=subprocess.PIPE)
        self.addCleanup(self.reap, process)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if process.poll() is not None:
                self.fail(f"supervisor exited early: {process.stderr.read()!r}")
            if self.state.exists():
                data = json.loads(self.state.read_text())
                if Path(data["active_file"]).exists():
                    return process, data
            time.sleep(.05)
        self.fail("supervisor did not start")

    @staticmethod
    def reap(process):
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        if process.stderr:
            process.stderr.close()

    def test_normal_shutdown_and_health(self):
        process, state = self.start()
        self.assertTrue(health.healthy(self.state, self.sock))
        with mock.patch.object(health, "proc_starttime", return_value=-1):
            self.assertFalse(health.healthy(self.state, self.sock))
        process.send_signal(signal.SIGTERM)
        self.assertEqual(process.wait(timeout=8), 0)
        self.assertFalse(self.state.exists())
        self.assertFalse(self.sock.exists())
        self.assertTrue(Path(state["active_file"]).exists())

    def test_child_failure_stops_supervisor(self):
        process, state = self.start()
        os.kill(state["atop_pid"], signal.SIGKILL)
        self.assertEqual(process.wait(timeout=8), 1)
        self.assertFalse(self.state.exists())
        self.assertFalse(self.sock.exists())

    def test_retention_only_own_regular_old_logs(self):
        self.logs.mkdir()
        current = self.logs / "atop_20261013_100"
        old = self.logs / "atop_20261001_100"
        other = self.logs / "other_20261001_100"
        link = self.logs / "atop_20261001_101"
        for path in (current, old, other):
            path.write_bytes(b"test")
            os.utime(path, (0, 0))
        link.symlink_to(old)
        supervisor.prune_logs(self.logs, current, 7,
                              dt.datetime(2026, 10, 13, tzinfo=dt.timezone.utc))
        self.assertTrue(current.exists())
        self.assertFalse(old.exists())
        self.assertTrue(other.exists())
        self.assertTrue(link.is_symlink())

    def test_utc_rotation_starts_new_log_and_stops_old_atop(self):
        self.logs.mkdir()
        fake_netatop = mock.Mock(pid=os.getpid(), poll=mock.Mock(return_value=None))
        fake_atops = []

        def popen(argv):
            if len(argv) == 1:
                return fake_netatop
            Path(argv[3]).write_bytes(b"sample")
            child = mock.Mock(pid=os.getpid(), poll=mock.Mock(return_value=None))
            fake_atops.append((argv, child))
            return child

        day1 = dt.datetime(2026, 10, 13, 23, 59, tzinfo=dt.timezone.utc)
        day2 = dt.datetime(2026, 10, 14, 0, 0, tzinfo=dt.timezone.utc)
        with mock.patch.dict(os.environ, self.env), \
             mock.patch.object(supervisor.subprocess, "Popen", side_effect=popen), \
             mock.patch.object(supervisor, "wait_socket"), \
             mock.patch.object(supervisor, "utc_now", side_effect=[day1, day2]), \
             mock.patch.object(supervisor.time, "sleep", side_effect=[None, RuntimeError("end test")]), \
             mock.patch.object(supervisor.signal, "signal"), \
             mock.patch.object(supervisor, "stop_child") as stop:
            with self.assertRaisesRegex(RuntimeError, "end test"):
                supervisor.run()
        self.assertEqual(len(fake_atops), 2)
        self.assertIn("atop_20261013_", fake_atops[0][0][3])
        self.assertIn("atop_20261014_", fake_atops[1][0][3])
        stop.assert_any_call(fake_atops[0][1], signal.SIGUSR2)


if __name__ == "__main__":
    unittest.main()
