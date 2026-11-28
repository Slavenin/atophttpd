#!/usr/bin/env python3
"""HTTP contract checks against synthetic atop 2.13 rawlog records."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
from urllib.error import HTTPError, URLError
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parent.parent.parent
PRODUCER = ROOT / "tests/fixtures/mkmetrics"
SERVER = Path(os.environ.get("ATOPHTTPD_BIN", ROOT / "atophttpd"))


def fixture(path, mode, timestamp):
    subprocess.run([str(PRODUCER), str(path), mode, str(timestamp)], check=True)


def request(port, path):
    try:
        with urlopen(f"http://127.0.0.1:{port}/{path}", timeout=6) as result:
            return result.status, result.read().decode()
    except HTTPError as error:
        return error.code, error.read().decode()


class RunningServer:
    def __init__(self, logdir, stale_after=0):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        self.proc = subprocess.Popen(
            [str(SERVER), "-P", str(logdir), "-p", str(self.port),
             "-S", str(stale_after)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        for _ in range(60):
            if self.proc.poll() is not None:
                raise AssertionError(f"server exited: {self.proc.stderr.read().decode()}")
            try:
                if request(self.port, "ping")[0] == 200:
                    break
            except (URLError, TimeoutError):
                time.sleep(0.05)
        else:
            raise AssertionError("server did not listen")

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc.stderr.close()


def run_case(logdir, check, stale_after=0):
    server = RunningServer(logdir, stale_after)
    try:
        check(server.port)
    finally:
        server.close()


def assert_no_process(body):
    assert "atophttpd_process_network_receive_bytes_per_second{" not in body
    assert "atophttpd_process_network_transmit_bytes_per_second{" not in body


def process_lines(body):
    return [line for line in body.splitlines() if line.startswith("atophttpd_process_network_")
            and "{" in line]


def main():
    now = int(time.time())
    with tempfile.TemporaryDirectory() as tmp:
        logdir = Path(tmp)

        def empty(port):
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_sample_fresh 0\n" in body
            assert "atophttpd_process_network_available 0\n" in body
            assert_no_process(body)
            assert request(port, "health")[0] == 503
            assert request(port, "ping")[0] == 200
        run_case(logdir, empty)

        path = logdir / "sample.raw"
        fixture(path, "cgroups", now - 2)

        def cgroups(port):
            status, body = request(port, "metrics")
            assert status == 200, (status, body)
            assert "atophttpd_sample_fresh 1\n" in body
            assert "atophttpd_process_network_available 1\n" in body
            assert 'pid="4242",name="a\\"\\\\b",start_time="1000",protocol="tcp"} 100.000000' in body
            assert 'pid="4242",name="second",start_time="2000",protocol="tcp"} 50.000000' in body
            assert body.count('atophttpd_process_network_receive_bytes_per_second{') == 4
            assert body.count('atophttpd_process_network_transmit_bytes_per_second{') == 4
            repeated_status, repeated_body = request(port, "metrics")
            assert repeated_status == 200
            assert process_lines(repeated_body) == process_lines(body)
            assert request(port, "health") == (200, "ok\n")
            assert process_lines(request(port, "metrics")[1]) == process_lines(body)
            status, response = request(port, f"showsamp?timestamp={now - 2}&labels=PRG,PRN&encoding=none")
            sample = json.loads(response)
            assert status == 200 and sample["timestamp"] == now - 2
            assert len(sample["PRG"]) == 2 and len(sample["PRN"]) == 2
            assert sample["PRG"][0]["name"] == '(a##b)'
            status, response = request(port, f"showsamp?timestamp={now - 2}&lables=PRN&encoding=none")
            assert status == 200 and len(json.loads(response)["PRN"]) == 2
            status, response = request(port, f"showsamp?timestamp={now - 12}&lables=PRN&encoding=none")
            assert status == 200 and json.loads(response)["timestamp"] == now - 12
            assert process_lines(request(port, "metrics")[1]) == process_lines(body)
        run_case(logdir, cgroups)

        for mode, expected in (("none", "atophttpd_process_network_available 0\n"),
                               ("boot", "atophttpd_process_network_available 1\n"),
                               ("zero-interval", "atophttpd_sample_fresh 0\n")):
            fixture(path, mode, now - 2)
            def suppressed(port):
                status, body = request(port, "metrics")
                assert status == 200 and expected in body, (mode, status, body)
                assert_no_process(body)
                assert request(port, "health")[0] == (200 if mode == "boot" else 503)
            run_case(logdir, suppressed)

        fixture(path, "rr-netatop", now - 2)
        def legacy_collector(port):
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_process_network_available 1\n" in body
            assert body.count('atophttpd_process_network_receive_bytes_per_second{') == 4
        run_case(logdir, legacy_collector)

        fixture(path, "cgroups", now - 120)
        def stale(port):
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_sample_fresh 0\n" in body
            assert_no_process(body)
            assert request(port, "health")[0] == 503
        run_case(logdir, stale)

        fixture(path, "cgroup-single", now + 1)
        def ages_without_file_change(port):
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_sample_fresh 1\n" in body
            assert process_lines(body)
            assert request(port, "health")[0] == 200
            time.sleep(3.1)
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_sample_fresh 0\n" in body
            assert_no_process(body)
            assert request(port, "health")[0] == 503
        run_case(logdir, ages_without_file_change, stale_after=1)

        for mode in ("bad-zlib", "bad-abi", "bad-length"):
            fixture(path, mode, now - 2)
            def invalid(port):
                status, _ = request(port, "metrics")
                assert status == 503, (mode, status)
                assert request(port, "health")[0] == 503
            run_case(logdir, invalid)

        with tempfile.NamedTemporaryFile(delete=False) as temporary:
            full = Path(temporary.name)
        fixture(full, "cgroup-single", now - 2)
        fixture(path, "partial", now - 2)
        def appended(port):
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_sample_fresh 0\n" in body
            with path.open("ab") as output, full.open("rb") as source:
                source.seek(path.stat().st_size)
                shutil.copyfileobj(source, output)
            time.sleep(3.1)
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_sample_fresh 1\n" in body
            fixture(full, "bpf", now - 2)
            os.replace(full, path)
            time.sleep(3.1)
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_process_network_available 1\n" in body
            fixture(full, "none", now - 2)
            assert full.stat().st_size == path.stat().st_size
            os.replace(full, path)  # equal-size inode replacement
            time.sleep(3.1)
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_process_network_available 0\n" in body
            previous = path.stat()
            fixture(path, "bpf", now - 2)
            os.utime(path, ns=(previous.st_atime_ns, previous.st_mtime_ns))
            assert path.stat().st_ino == previous.st_ino
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_process_network_available 1\n" in body
            path.write_bytes(path.read_bytes()[:256])
            time.sleep(3.1)
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_sample_fresh 0\n" in body
            path.unlink()
            time.sleep(3.1)
            status, body = request(port, "metrics")
            assert status == 200 and "atophttpd_sample_fresh 0\n" in body
        run_case(logdir, appended)

        append_time = int(time.time())
        fixture(path, "cgroup-single", append_time - 2)
        fixture(full, "cgroups", append_time + 8)
        first_length = path.stat().st_size
        all_bytes = full.read_bytes()
        assert path.read_bytes() == all_bytes[:first_length]
        def partial_tail(port):
            status, body = request(port, "metrics")
            assert status == 200 and f"atophttpd_sample_timestamp_seconds {append_time - 2}\n" in body
            with path.open("ab") as output:
                output.write(all_bytes[first_length:first_length + 128])
            status, body = request(port, "metrics")
            assert status == 200 and f"atophttpd_sample_timestamp_seconds {append_time - 2}\n" in body
            with path.open("ab") as output:
                output.write(all_bytes[first_length + 128:])
            status, body = request(port, "metrics")
            assert status == 200 and f"atophttpd_sample_timestamp_seconds {append_time + 8}\n" in body
            assert request(port, "health")[0] == 200
        run_case(logdir, partial_tail)
        path.unlink()
        full.unlink()

        fixture(logdir / "z_older.raw", "cgroups", now - 8)
        fixture(logdir / "a_newer.raw", "cgroups", now - 2)
        def latest(port):
            status, body = request(port, "metrics")
            assert status == 200 and f"atophttpd_sample_timestamp_seconds {now - 2}\n" in body
        run_case(logdir, latest)

        older = logdir / "z_older.raw"
        newer = logdir / "a_newer.raw"
        def newest_corrupt(port):
            status, body = request(port, "metrics")
            assert status == 200 and f"atophttpd_sample_timestamp_seconds {now - 2}\n" in body
            # A read failure must stay attached to the newest source until repair.
            newer.chmod(0)
            try:
                assert request(port, "metrics")[0] == 503
                assert request(port, "metrics")[0] == 503
            finally:
                newer.chmod(0o644)
            assert request(port, "metrics")[0] == 200
            for mode in ("bad-zlib", "bad-length", "bad-magic"):
                fixture(newer, mode, now - 2)
                assert request(port, "metrics")[0] == 503, mode
                assert request(port, "health")[0] == 503, mode
                fixture(newer, "cgroups", now - 2)
                status, body = request(port, "metrics")
                assert status == 200 and f"atophttpd_sample_timestamp_seconds {now - 2}\n" in body, (mode, now - 2, now - 8, status, body[:300])
            fixture(older, "bad-abi", now - 8)
            time.sleep(3.1)
            assert request(port, "metrics")[0] == 200
        run_case(logdir, newest_corrupt)

    print("integration: rawlog, JSON, metrics, cache changes OK")


class MetricsIntegration(unittest.TestCase):
    def test_metrics_contract(self):
        main()


if __name__ == "__main__":
    unittest.main()
