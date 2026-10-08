# Contributing to VeloCache

VeloCache explores how a small macOS key-value server handles protocols, durability, expiration, and replication. Focused fixes, reproducible bug reports, and clearer explanations are welcome.

## Development

Use macOS with Xcode Command Line Tools, Python 3, and Node.js 26 for the TypeScript client checks. No third-party networking library is required.

```bash
make
make test
python3 benchmarks/run.py --requests 200
```

Tests use temporary storage and local ports. Never run experiments against an AOF containing data you need.

## Pull requests

Open an issue before a large architectural change. For small fixes, submit a pull request directly. Explain the concrete behavior before and after the change, include a minimal reproduction when relevant, and report which checks you ran. Keep unrelated formatting and generated binaries out of the diff.

Protocol, persistence, and replication changes should include regression coverage. Preserve the distinction between empty values and missing keys, and preserve the rule that a successful SET response follows durable storage. The project currently keeps code free of explanatory inline comments; use clear names and documentation for design explanations.

## Useful first contributions

- Add focused tests for malformed RESP frames and fragmented requests.
- Improve command examples and explain observed error responses.
- Reproduce a bug with a small test before proposing a fix.
- Measure a workload with the benchmark methodology recorded alongside the result.

Performance claims need reproducible commands, hardware and operating system details, durability settings, and an explanation of client limitations. Do not compare unlike durability settings.
