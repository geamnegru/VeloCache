# Reproducible local benchmark

```bash
make
python3 benchmarks/run.py --requests 2000 --output benchmarks/local.json
```

The script starts an isolated server with a temporary AOF and removes it on exit. It verifies every response. Each workload has 50 warmup requests, then the requested number of measured operations. PING, GET, and SET run sequentially over one persistent RESP2 connection, without pipelining. Values contain 64 bytes. SET includes the server's fsync-before-acknowledgment behavior.

The JSON records throughput and nearest-lower-rank latency percentiles in milliseconds. Client and server run on the same machine. Python, TCP loopback, scheduling, filesystem caching, and the host's storage affect these results. This is a smoke benchmark and a reproducible baseline, not a server saturation test or a comparison with Redis.

`baseline.json` records one local development run on an Apple M4, macOS 27.0 arm64, with Apple clang 21.0.0 and the Makefile defaults (`-std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread`). The source included uncommitted development changes on top of `0ea97ba`; this is not a tagged-release measurement. `fsync` is the server durability primitive; this benchmark does not establish power-loss guarantees for the hardware. Repeat runs and record hardware, compiler, commit, and durability settings before making public performance claims. CI uses only 50 requests to verify that the benchmark works; its timings are not performance gates.
