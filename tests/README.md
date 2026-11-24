# HTTP integration checks

Run `make test` from the repository root. The Makefile compiles `mkfixture.c` against the pinned atop headers, starts atophttpd on a temporary loopback port, and runs `test_integration.py` against `/metrics` and `/showsamp`.

The fixtures cover native ABI rejection, four-block cgroup record indexing, partial append, file replacement, stale and boot samples, collector flags, process/thread filtering, and JSON escaping. They use synthetic records and need no privileged collector. For a real process-network check, record a log with atop `-K` and a collector compatible with the host kernel, then compare `/metrics` with `atop -r FILE -P PRN`. Use `-S` only if the saved log is older than the normal freshness limit.
