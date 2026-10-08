# VeloCache

[![macOS CI](https://github.com/geamnegru/VeloCache/actions/workflows/ci.yml/badge.svg)](https://github.com/geamnegru/VeloCache/actions/workflows/ci.yml)

**Explore a small durable key-value server, from TCP frames to replicated storage.**

VeloCache is a C++17 key-value server for macOS with RESP2 support, AOF persistence, and asynchronous master/slave replication. A single `kqueue` event loop handles nonblocking TCP sockets without creating a thread for each client.

The server and C++ test executables enable fast stream I/O with `std::ios_base::sync_with_stdio(false)` and `std::cin.tie(nullptr)`. Startup and synchronization messages are explicitly flushed so they appear immediately.

Supported commands are `PING`, `SET`, `GET`, and `SET ... EX seconds`. RESP keys and values can contain spaces, newlines, and NUL bytes. Expiration deadlines survive restarts and replication.

## Try it in two minutes

On macOS with Xcode Command Line Tools installed:

```bash
git clone https://github.com/geamnegru/VeloCache.git
cd VeloCache
make
./velocache --bind 127.0.0.1 --port 6379 --aof demo.aof
```

In another terminal:

```bash
printf 'SET greeting hello\n' | nc 127.0.0.1 6379
printf 'GET greeting\n' | nc 127.0.0.1 6379
printf 'PING\n' | nc 127.0.0.1 6379
```

Expect `OK`, `hello`, and `PONG`. Restart the server with the same AOF to recover the value. Use a trusted local environment: authentication and TLS are not implemented.

VeloCache is an experimental systems project with a small Redis-compatible command subset. It currently targets macOS; Linux support and the full Redis API are not implemented.

[Benchmark methodology](benchmarks/README.md) · [Contributing](CONTRIBUTING.md) · [CI results](https://github.com/geamnegru/VeloCache/actions)

## Building

The server uses the C++ standard library and POSIX/macOS APIs. Building requires Command Line Tools and a C++17 compiler.

```bash
make
```

Equivalent compiler command:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread main.cpp storage.cpp client_handler.cpp event_loop.cpp server.cpp resp.cpp background_worker.cpp -o velocache
```

## Running a master and a slave

In the first terminal:

```bash
./velocache --bind 127.0.0.1 --port 6379 --aof master.aof
```

In the second terminal:

```bash
./velocache --bind 127.0.0.1 --port 6380 --aof slave.aof --replicaof 127.0.0.1 6379
```

Each process uses its own AOF file. The slave prints `Replica synchronized` after installing a complete snapshot.

| Option | Default | Purpose |
| --- | --- | --- |
| `--bind HOST` | `0.0.0.0` | IPv4 address on which the server listens |
| `--port PORT` | `6379` | Local port |
| `--aof PATH` | `velocache.aof` | Persistent journal |
| `--replicaof HOST PORT` | Not set | Run the process as a slave |
| `--help` | — | Display available options |

Stop the process with `Ctrl+C`. Restart it with the same `--aof` path to recover its data.

## RESP2

Requests are RESP2 arrays of bulk strings. The server measures lengths in bytes and waits for a complete frame before executing a command. Responses preserve values exactly, including control characters.

| Command | RESP2 response |
| --- | --- |
| `PING` | `+PONG\r\n` |
| `PING message` | Bulk string containing the message |
| `SET key value` | `+OK\r\n` |
| `SET key value EX seconds` | `+OK\r\n` |
| `GET key` for an existing key | `$<bytes>\r\n<value>\r\n` |
| `GET key` for a missing or expired key | `$-1\r\n` |
| Invalid command | `-ERR ...\r\n` |
| Write to a slave | `-READONLY ...\r\n` |
| Read from a slave while synchronizing | `-LOADING ...\r\n` |

An empty value returns `$0\r\n\r\n`, which is distinct from a missing key. Command names and the `EX` option are case-insensitive. TTL must be a positive integer. A new `SET` without `EX` removes any previous expiration.

RESP connections remain open for multiple commands. Pipelining allows clients to send multiple requests before reading their responses, which are delivered in order. Requests fragmented across TCP reads are accumulated. Command errors keep the connection open; an invalid RESP frame produces an error and closes the connection after sending responses already prepared.

The server implements the RESP2 subset needed for these commands. RESP3, `HELLO` negotiation, authentication, transactions, Pub/Sub, and other Redis commands are not implemented. The internal replication protocol is separate and uses text frames with checksums.

Reference: [official RESP specification](https://redis.io/docs/latest/develop/reference/protocol-spec/).

### TypeScript client

`client.ts` uses only Node.js's built-in `net` module, with no external dependencies. It builds RESP2 requests and decodes fragmented responses using UTF-8 byte lengths. Each call opens a socket and destroys it after receiving a response; the inactivity timeout is five seconds.

With the master running:

```bash
node client.ts
```

The example exercises `PING`, `SET`, `GET`, and expiration with `EX 1`. This command has been verified with Node.js 26, which executes this TypeScript directly.

The class can be imported without running the example:

```typescript
import { VeloClient } from './client.ts';

const client = new VeloClient(6379, '127.0.0.1');
await client.set('message', 'Hello\nVeloCache', 10);
console.log(await client.get('message'));
```

The public API returns `Promise<string>`; missing keys are represented by the string `'(nil)'`, and RESP errors reject the promise.

### Testing with redis-cli

If `redis-cli` is already installed, select RESP2 mode:

```bash
redis-cli -2 -h 127.0.0.1 -p 6379 PING
redis-cli -2 -h 127.0.0.1 -p 6379 SET demo "saved on the master"
redis-cli -2 -h 127.0.0.1 -p 6380 GET demo
redis-cli -2 -h 127.0.0.1 -p 6379 SET temporary "expires in two seconds" EX 2
```

The `-2` option is documented in the [official redis-cli documentation](https://redis.io/docs/latest/manual/cli/). Compatibility is limited to the implemented commands.

### Existing text commands

Connections beginning with a text command retain the original protocol: one newline-terminated command, one text response, and then connection closure. Parsing with `stringstream` and removal of trailing `\n`/`\r` characters are preserved.

```bash
printf 'SET demo saved on the master\n' | nc 127.0.0.1 6379
printf 'GET demo\n' | nc 127.0.0.1 6380
printf 'SET demo modified on the slave\n' | nc 127.0.0.1 6380
```

After asynchronous propagation, reading from the slave returns `saved on the master`. Writing to the slave returns `ERR Read only replica`. Use RESP for keys or values containing newlines or NUL bytes.

## AOF persistence

Each write contains a sequence number, key, value, and absolute expiration deadline in Unix milliseconds. Keys and values are hex-encoded, and a 64-bit FNV-1a checksum verifies record integrity. The versioned header is `VCAOF1`.

The server writes the complete record and calls `fsync` before returning `OK`. On restart, it reconstructs state from the journal; time spent offline counts toward TTL. `GET` checks expiration immediately. An incomplete final record is truncated; a complete corrupted record prevents startup and produces an explicit error.

The `<aof>.lock` file prevents two processes from using the same journal. A snapshot received by a slave is written to a temporary file, synchronized, and installed with `rename` and directory synchronization before becoming visible. Persistence errors stop the process before it acknowledges a new write.

To test recovery manually, set a key, stop the master, restart it with the same `--aof` path, and read the key. `make clean` preserves AOF files.

## Expiration index

`StorageEngine` stores expiration deadlines in a `std::multimap<std::int64_t, std::string>`, ordered by absolute deadline. Each value in the primary map keeps an iterator to its own expiration entry. Overwriting a key, removing its TTL, or expiring it through `GET` removes the old entry by iterator without searching the multimap.

For `E` keys with TTL, inserting an expiration takes `O(log E)`, while erasing by iterator takes amortized `O(1)`. Cleanup checks the earliest deadline and removes only expired entries: expected `O(K)` for `K` expirations, using expected `O(1)` access in `unordered_map`. If nothing has expired, the check takes `O(1)`. Cleanup does not scan all keys; simultaneous expirations have separate entries in the multimap.

AOF replay rebuilds the index from the final state. Snapshot replacement prepares and swaps both the data and the index together, preventing obsolete deadlines from deleting new values.

## Background storage and disk writes

The server uses a `kqueue` network loop and one `BackgroundWorker` for storage. The worker executes data-access commands, AOF writes, `fsync`, TTL cleanup, snapshot generation and installation, and incoming replication updates in FIFO order. Reads use the same queue so the network loop does not wait for the storage mutex.

Results are delivered to the network thread through a nonblocking `socketpair` registered with `kqueue`. The server sends `OK` and propagates mutations to replicas only after persistence completes. `PING` on another connection remains available during a slow `fsync`. Commands and responses on the same connection remain ordered, including pipelined requests.

Each connection has at most one storage operation in flight. The queue enforces an accounting budget of 128 MiB and at most 1024 operations, including results awaiting delivery. This budget reserves space for responses and is not an exact limit on process memory. When the queue is full, commands receive `ERR`, and replica synchronization is retried. Connection identities prevent results from being delivered to reused sockets.

During normal shutdown, the worker finishes accepted operations and is joined. An I/O error is forwarded to the main loop and stops the process without acknowledging the failed write.

## Asynchronous replication

The master accepts writes and can serve multiple slaves. Each slave opens a persistent connection and requests `SYNC`; the master sends `SNAP_BEGIN`, snapshot entries, and `SNAP_END`, followed by ordered `UPDATE` mutations.

The slave validates checksums and sequence numbers. It installs a snapshot only after the transfer is complete and persists updates in its own AOF. Binary keys and values are preserved. TTL is transferred as an absolute deadline and assumes synchronized system clocks across nodes.

After disconnection, existing data remains on disk, while reads return `LOADING` in RESP or `ERR Replica syncing` in text mode until synchronization completes. `PING` remains available. Reconnection is attempted every 500 ms and requests a new snapshot. Heartbeats check the sequence once per second; five seconds without activity triggers reconnection. This timeout is suspended while the connection awaits its own storage operation so that persistence in progress is not canceled.

`OK` confirms the master's journal. Immediate reads from a slave may see an earlier state until propagation completes. Automatic promotion, master election, and slave acknowledgments of writes are not implemented.

## Project files

| Files | Responsibility |
| --- | --- |
| `storage.h`, `storage.cpp` | Data, multimap TTL index, internal codec, AOF, snapshots, and locking |
| `background_worker.h`, `background_worker.cpp` | FIFO queue, storage thread, and kqueue notifications |
| `resp.h`, `resp.cpp` | RESP2 request parsing and response encoding |
| `client_handler.h`, `client_handler.cpp` | Text and RESP command execution |
| `event_loop.h`, `event_loop.cpp` | Native kqueue adapter |
| `server.h`, `server.cpp` | Accepting connections, partial reads/writes, pipelining, and replication |
| `main.cpp` | CLI options, signals, and startup |
| `client.ts` | Native Node.js RESP2 client and example |
| `tests/` | Storage, protocol, and integration tests |

## Limits

Keys, values, and individual bulk strings are limited to 1 MiB. A RESP request can contain at most 64 arguments and `4 MiB + 256` bytes. A RESP length header is limited to 32 bytes, including its marker and CRLF.

Each connection's output buffer and each snapshot transfer are limited to 64 MiB. A replica that falls too far behind or a snapshot exceeding this limit causes disconnection and a retry. The server accepts up to 4096 connections, subject to available file descriptors. Inactive public connections are closed after 30 seconds.

AOF recovery at startup is synchronous and runs before accepting clients. During normal operation, disk I/O and snapshots run in the worker and do not block the kqueue loop. A slow disk delays queued storage operations, including reads; there is no group commit or pool of writers. AOF grows with each write; periodic compaction of the master's journal is not implemented.

This version runs on macOS and does not include authentication or TLS.

## Tests

Tests require Python 3 and, for client verification, Node.js with support for executing TypeScript.

```bash
make test
```

Tests use temporary directories and available ports. They cover:

- RESP2, pipelining, persistent connections, fragmented frames, and socket half-close;
- empty values, missing keys, UTF-8, CRLF, and NUL bytes;
- invalid arguments and TTL, frame limits, and protocol errors;
- persistence after `SIGKILL`, absolute TTL, and incomplete AOF tail repair;
- corrupted checksums, sequence validation, and interrupted snapshots;
- replication, read-only mode, reconnection, and the TypeScript client;
- simultaneous partial connections and temporary file descriptor exhaustion;
- simultaneous expirations, TTL overwrites, and index reconstruction;
- FIFO ordering, backpressure, and controlled worker shutdown;
- artificially delayed `fsync`, `PING` availability, and I/O failures without acknowledgment.

The slow-disk test builds a macOS interposition library used only by the test process. It delays `fsync` by 800 ms and can inject `EIO`; the production binary contains no test hooks.

## License

VeloCache is licensed under the [MIT License](LICENSE).
