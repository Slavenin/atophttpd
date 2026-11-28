# Tests

Run `make check` from the repository root. Upstream C unit tests cover cache,
rawlog, JSON, HTTP writes and connection handling; Python integration tests
exercise the server over loopback. `fixtures/mkmetrics.c` writes native atop
2.13 records for `integration/test_metrics.py`, including BPF flags, cgroups,
partial appends and corrupted records. `unit/test_metrics.c` checks freshness,
escaping and process-block generation. These fixtures need no BPF privileges.

The metrics integration test checks `/metrics`, `/health`, `/showsamp` and
their ordering, including a JSON request between two scrapes. It also tests
file replacement, same-inode rewrites, truncation, unreadable newest logs and
repair. To check a real collector, record with atop `-K` and compare `/metrics`
with `atop -r FILE -P PRN`. A saved log older than the normal freshness cutoff
needs `-S SECONDS` for that comparison.

The additional quality gates are `make sanitize`, `make coverage COV=90`,
`make tsan` and `make fuzz FUZZ_SECONDS=30`. They use the same source and native
atop submodule; `SKIP_SUBMODULE_UPDATE=1` runs them from a prepared checkout.
