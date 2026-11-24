# atophttpd

atophttpd reads [atop](https://www.atoptool.nl/) raw logs and serves the existing browser view, JSON samples, and Prometheus metrics. This checkout uses atop **2.13.0** (the pinned `atop` submodule). Raw logs must have that version's native layout; convert older files with `atopconvert` before placing them in the log directory.

## Build

You need a C compiler, make, zlib headers, and Git for the submodule. TLS builds also need OpenSSL headers and libraries. A fresh checkout builds with:

```sh
make -j4
make test
```

`make` initializes the pinned submodule if it is missing. `make USE_TLS=YES -j4` builds with HTTPS support; switching back to `make -j4` selects the plain build. `make install` installs the binary, systemd unit, and man page under `/usr`. Use `DESTDIR=/path/to/stage` to stage those files. `make deb` builds a package for the current Debian architecture.

## Collect process traffic

atop needs a compatible process-network collector (`netatop` or `netatop-bpf`) and must start with `-K`. Start the collector before atop opens a new raw log. For a foreground check:

```sh
sudo atop -K -w "/var/log/atop/atop_$(date +%Y%m%d)" 10
```

For the upstream `atop.service`, set `LOGOPTS="-K"` and `LOGINTERVAL=10` in `/etc/default/atop`, then restart the service. Check the raw log with `atop -r /var/log/atop/atop_YYYYMMDD -P PRN`. A compatible collector is kernel-dependent; this project does not install or manage one. The BPF capability bit is stored in the raw-log header, so a log opened before the collector was active may show `atophttpd_process_network_available 0` even after the collector starts.

Run the server against the log directory:

```sh
./atophttpd -P /var/log/atop -p 2867
curl http://127.0.0.1:2867/metrics
```

The default HTTP bind address is `127.0.0.1`. If Prometheus runs in Docker, bind `-a` to a host address reachable from that container. The [example scrape job](examples/prometheus.yml) uses `host.docker.internal:2867`; on Linux its container needs `host.docker.internal:host-gateway` or equivalent routing. Keep the exporter on a trusted interface because the browser and JSON endpoints expose process information.

## Prometheus and Grafana

`/metrics` exports interval **gauges** in bytes per second, split by `host`, `pid`, `name`, `start_time` (the process start epoch), and `protocol` (`tcp` or `udp`). Use these expressions in a Grafana time-series panel to show receive and transmit traffic grouped by process name:

```promql
sum by (instance, name) (atophttpd_process_network_receive_bytes_per_second)
sum by (instance, name) (atophttpd_process_network_transmit_bytes_per_second)
```

For separate process lifetimes, group by `(instance, pid, name, start_time)` instead. These values already divide atop's per-interval byte counts by the interval; do not apply `rate()` to them. Several Prometheus scrapes within one atop interval repeat the same value. A 10-second atop interval and 10-second Prometheus scrape are a useful starting point.

Only the latest complete sample is exported. A scrape interval longer than the atop interval can miss samples and short-lived processes; the exporter does not replay older intervals into Prometheus.

The exporter also reports `atophttpd_sample_timestamp_seconds`, `atophttpd_sample_interval_seconds`, `atophttpd_sample_age_seconds`, `atophttpd_sample_fresh`, and `atophttpd_process_network_available`. The availability metric says that the raw log records process-network data; it does not check whether a collector is currently healthy. Process series disappear when the sample is stale, has a zero interval, is a boot sample, or has no process-network data. By default, a sample is stale after `max(30, 2 × interval)` seconds; `-S SECONDS` overrides that threshold. With no complete sample, `/metrics` still returns status gauges with freshness and availability set to zero. A malformed or unreadable indexed sample returns HTTP 503.

`/health` returns `ok` with HTTP 200 when the latest sample is fresh and records process-network data; it returns 503 otherwise. Unlike `/metrics`, it does not render process series. Both endpoints recheck the rawlog file on each request. The exporter keeps one decoded sample and one formatted process-metrics block in memory while its file identity and metadata remain unchanged; age and freshness are recalculated for every request. The 3-second directory scan uses a monotonic clock, so appends and replacements of the selected file are checked immediately while discovery of a new file may take up to 3 seconds.

## Browser and JSON

Open `http://127.0.0.1:2867/` for the browser view or `/help` for request syntax. JSON remains available at `/showsamp?timestamp=EPOCH&lables=ALL&encoding=none`. The original `lables` parameter is preserved; `labels` is an alias. The `PRN` section contains per-process network interval counts when the raw log supports them.

For mutual TLS, build with `USE_TLS=YES`, generate test certificates with `bash gen-cert.sh`, and start with `-t 2868 -C tls/ca.crt -c tls/server.crt -k tls/server.key`. Set `-A` explicitly for the desired TLS bind address. The certificate script creates local test material, not a production certificate authority.

For this machine's Docker Compose monitoring stack, see the [collector and Prometheus example](examples/monitoring-stack/README.md). It builds the local kernel-compatible `netatop-bpf` fork and atop 2.13.0 in containers, with no host installation.

## Code map

`rawlog.c` verifies the pinned atop ABI, scans complete records (system, process, cgroup statistics, and cgroup PID list), and owns the borrowed decoded sample used by `/metrics`, `/health`, and `/showsamp`. `cache.c` indexes timestamps and tracks file identity so append, truncation, and replacement are noticed. `sample.h` holds the decoded sample and status interfaces. `metrics.c` formats process series once per sample generation and recalculates status fields on each request; `json.c` renders `/showsamp`. `httpd.c` throttles directory scans with `CLOCK_MONOTONIC` and dispatches HTTP/TLS connections. The fixture in `tests/mkfixture.c` writes native atop 2.13 records; `make test` checks HTTP, cache transitions, and clock/status behavior.
