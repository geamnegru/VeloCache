import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from integration import HOST, Server, eventually, expect, free_ports, require


def encode_request(*arguments):
    fields = [argument.encode("utf-8") if isinstance(argument, str) else argument for argument in arguments]
    return b"*" + str(len(fields)).encode("ascii") + b"\r\n" + b"".join(
        b"$" + str(len(field)).encode("ascii") + b"\r\n" + field + b"\r\n"
        for field in fields
    )


class RespConnection:
    def __init__(self, port, timeout=3.0):
        self.socket = socket.create_connection((HOST, port), timeout=timeout)
        self.socket.settimeout(timeout)
        self.buffer = bytearray()

    def __enter__(self):
        return self

    def __exit__(self, *arguments):
        self.socket.close()

    def fill(self, count):
        while len(self.buffer) < count:
            chunk = self.socket.recv(65536)
            require(bool(chunk), "RESP connection closed before a complete response")
            self.buffer.extend(chunk)

    def line(self):
        while True:
            boundary = self.buffer.find(b"\r\n")
            if boundary >= 0:
                result = bytes(self.buffer[:boundary])
                del self.buffer[:boundary + 2]
                return result
            require(len(self.buffer) < 65536, "Response header exceeded expected length")
            self.fill(len(self.buffer) + 1)

    def receive(self):
        line = self.line()
        require(bool(line), "Empty RESP response header")
        marker, value = line[:1], line[1:]
        if marker in (b"+", b"-"):
            return marker, value
        require(marker == b"$", f"Unsupported RESP response: {line!r}")
        length = int(value)
        if length == -1:
            return marker, None
        require(length >= 0, f"Invalid response bulk length: {length}")
        self.fill(length + 2)
        require(self.buffer[length:length + 2] == b"\r\n", "Missing bulk response terminator")
        result = bytes(self.buffer[:length])
        del self.buffer[:length + 2]
        return marker, result

    def send(self, *arguments):
        self.socket.sendall(encode_request(*arguments))

    def request(self, *arguments):
        self.send(*arguments)
        return self.receive()

    def expect(self, arguments, expected):
        actual = self.request(*arguments)
        require(actual == expected, f"{arguments!r}: expected {expected!r}, got {actual!r}")

    def expect_closed(self):
        require(not self.buffer, f"Unexpected buffered data after responses: {self.buffer!r}")
        require(self.socket.recv(1) == b"", "Server did not close completed connection")


def response(port, *arguments):
    with RespConnection(port) as connection:
        return connection.request(*arguments)


def eventually_resp(port, arguments, expected, timeout=5.0):
    deadline = time.monotonic() + timeout
    actual = None
    while time.monotonic() < deadline:
        try:
            actual = response(port, *arguments)
            if actual == expected:
                return
        except (OSError, AssertionError) as error:
            actual = str(error)
        time.sleep(0.025)
    raise AssertionError(f"{arguments!r}: expected {expected!r}, last result {actual!r}")


def expect_protocol_error(port, frame):
    with RespConnection(port) as connection:
        connection.socket.sendall(frame)
        marker, message = connection.receive()
        require(marker == b"-" and message.startswith(b"ERR Protocol error"),
                f"Malformed request did not produce a protocol error: {(marker, message)!r}")
        connection.expect_closed()


def test_connections(master):
    expect(master.port, "PING", "PONG")
    binary_key = b"key with spaces\r\n\x00" + "și 世界".encode("utf-8")
    binary_value = b"value\r\nsecond line\x00" + "Bună 世界".encode("utf-8")
    with RespConnection(master.port) as connection:
        connection.expect(("PING",), (b"+", b"PONG"))
        connection.expect(("pInG", binary_value), (b"$", binary_value))
        connection.expect(("GET", "missing_resp"), (b"$", None))
        connection.expect(("SET", "", ""), (b"+", b"OK"))
        connection.expect(("GET", ""), (b"$", b""))
        connection.expect(("SET", binary_key, binary_value), (b"+", b"OK"))
        connection.expect(("GET", binary_key), (b"$", binary_value))
        connection.expect(("SET", "literal_nil", "(nil)"), (b"+", b"OK"))
        connection.expect(("GET", "literal_nil"), (b"$", b"(nil)"))
        connection.expect(("SET", "resp_canary", "from RESP"), (b"+", b"OK"))
        commands = [
            ("SET", "pipeline", "one"), ("GET", "pipeline"),
            ("SET", "pipeline", "two"), ("GET", "pipeline"),
            ("GET", "pipeline_missing"), ("PING",),
        ]
        expected = [(b"+", b"OK"), (b"$", b"one"), (b"+", b"OK"),
                    (b"$", b"two"), (b"$", None), (b"+", b"PONG")]
        connection.socket.sendall(b"".join(encode_request(*command) for command in commands))
        for wanted in expected:
            require(connection.receive() == wanted, "Pipeline replies changed order or framing")
        connection.expect(("GET", binary_key), (b"$", binary_value))
    with RespConnection(master.port) as connection:
        for byte in encode_request("SET", "fragmented_resp", binary_value):
            connection.socket.sendall(bytes([byte]))
            time.sleep(0.001)
        require(connection.receive() == (b"+", b"OK"), "Bytewise fragmented SET failed")
        connection.expect(("GET", "fragmented_resp"), (b"$", binary_value))
    with RespConnection(master.port) as connection:
        commands = [("PING", f"pipeline item {index}") for index in range(150)]
        connection.socket.sendall(b"".join(encode_request(*command) for command in commands))
        for index in range(150):
            require(connection.receive() == (b"$", f"pipeline item {index}".encode("ascii")),
                    "Pipeline stalled or reordered replies beyond one event-loop budget")
    expect(master.port, "GET resp_canary", "from RESP")
    print("PASS persistent RESP connections, ordered pipelines and binary fragmentation")
    return binary_key, binary_value


def test_command_errors(master):
    with RespConnection(master.port) as connection:
        connection.expect(("SET", "unchanged_resp", "original"), (b"+", b"OK"))
        commands = [
            ("PING", "a", "b"), ("GET",), ("GET", "a", "b"),
            ("SET", "key"), ("SET", "key", "value", "EX"),
            ("SET", "key", "value", "NX", "1"), ("UNKNOWN",),
        ]
        commands += [("SET", "unchanged_resp", "changed", "EX", ttl)
                     for ttl in ("0", "-1", "nope", "1x", "99999999999999999999999999")]
        for command in commands:
            marker, message = connection.request(*command)
            require(marker == b"-" and bool(message), f"Invalid command succeeded: {command!r}")
            connection.expect(("PING",), (b"+", b"PONG"))
        connection.expect(("GET", "unchanged_resp"), (b"$", b"original"))
        connection.expect(("SET", "ttl_resp", "expires", "ex", "1"), (b"+", b"OK"))
        connection.expect(("GET", "ttl_resp"), (b"$", b"expires"))
        connection.expect(("SET", "reset_resp", "temporary", "EX", "1"), (b"+", b"OK"))
        connection.expect(("SET", "reset_resp", "forever"), (b"+", b"OK"))
    eventually_resp(master.port, ("GET", "ttl_resp"), (b"$", None), timeout=2.5)
    require(response(master.port, "GET", "reset_resp") == (b"$", b"forever"),
            "SET without EX retained prior expiration")
    print("PASS command errors preserve connection and data, EX expiry and TTL reset")


def test_protocol_errors(master):
    for frame in (
        b"*0\r\n", b"*-1\r\n", b"*65\r\n", b"*x\r\n",
        b"*1\r\n$-1\r\n", b"*1\r\n$1048577\r\n",
        b"*1\r\n$184467440737095516160\r\n",
        b"*1\r\n+PING\r\n", b"*1\r\n$4\r\nPINGxx",
    ):
        expect_protocol_error(master.port, frame)
    with RespConnection(master.port) as connection:
        frame = encode_request("PING") + b"*1\r\n$-1\r\n" + encode_request("SET", "after_error", "forbidden")
        connection.socket.sendall(frame)
        require(connection.receive() == (b"+", b"PONG"), "Protocol error lost prior complete reply")
        marker, message = connection.receive()
        require(marker == b"-" and message.startswith(b"ERR Protocol error"), "Malformed pipeline tail did not fail")
        connection.expect_closed()
    require(response(master.port, "GET", "after_error") == (b"$", None),
            "Server processed a command after malformed protocol")
    expect(master.port, "PING", "PONG")
    print("PASS malformed RESP frames fail safely and close after queued replies")


def test_half_close(master):
    payload = b"large\x00\r\n" * 16384
    with RespConnection(master.port) as connection:
        commands = [("SET", "half_closed", payload), ("GET", "half_closed")]
        commands.extend(("PING",) for _ in range(150))
        connection.socket.sendall(b"".join(encode_request(*command) for command in commands))
        connection.socket.shutdown(socket.SHUT_WR)
        require(connection.receive() == (b"+", b"OK"), "Half-close lost SET reply")
        require(connection.receive() == (b"$", payload), "Half-close truncated a large bulk reply")
        for _ in range(150):
            require(connection.receive() == (b"+", b"PONG"), "Half-close lost a pipelined reply")
        connection.expect_closed()
    with RespConnection(master.port) as connection:
        incomplete = encode_request("SET", "incomplete_resp", "must not exist")[:-3]
        connection.socket.sendall(encode_request("PING") + incomplete)
        connection.socket.shutdown(socket.SHUT_WR)
        require(connection.receive() == (b"+", b"PONG"), "Incomplete EOF lost prior complete frame")
        marker, message = connection.receive()
        require(marker == b"-" and message.startswith(b"ERR Protocol error"),
                "Incomplete request at EOF did not produce an error")
        connection.expect_closed()
    require(response(master.port, "GET", "incomplete_resp") == (b"$", None),
            "Incomplete SET at EOF changed storage")
    print("PASS half-close drains complete pipelines and rejects incomplete writes")


def test_persistence(master, binary_key, binary_value):
    master.stop(kill=True)
    master.start()
    with RespConnection(master.port) as connection:
        connection.expect(("GET", binary_key), (b"$", binary_value))
        connection.expect(("GET", ""), (b"$", b""))
        connection.expect(("GET", "fragmented_resp"), (b"$", binary_value))
        connection.expect(("GET", "ttl_resp"), (b"$", None))
        connection.expect(("GET", "reset_resp"), (b"$", b"forever"))
        connection.expect(("GET", "incomplete_resp"), (b"$", None))
    print("PASS AOF restores binary and empty data after SIGKILL")


def test_replication(master, replica, binary_key, binary_value):
    replica.start()
    eventually_resp(replica.port, ("GET", binary_key), (b"$", binary_value))
    eventually_resp(replica.port, ("GET", ""), (b"$", b""))
    eventually(replica.port, "GET resp_canary", "from RESP")
    updated = binary_value + b"\x00updated\r\n"
    with RespConnection(master.port) as connection:
        connection.expect(("SET", binary_key, updated), (b"+", b"OK"))
        connection.expect(("SET", "", "new empty-key value"), (b"+", b"OK"))
        connection.expect(("SET", "replica_ttl_resp", "transient", "EX", "1"), (b"+", b"OK"))
    eventually_resp(replica.port, ("GET", binary_key), (b"$", updated))
    eventually_resp(replica.port, ("GET", ""), (b"$", b"new empty-key value"))
    eventually_resp(replica.port, ("GET", "replica_ttl_resp"), (b"$", b"transient"), timeout=0.75)
    with RespConnection(replica.port) as connection:
        marker, message = connection.request("SET", binary_key, "forbidden")
        require(marker == b"-" and bool(message), "Replica accepted a RESP write")
        connection.expect(("PING",), (b"+", b"PONG"))
        connection.expect(("GET", binary_key), (b"$", updated))
    eventually_resp(replica.port, ("GET", "replica_ttl_resp"), (b"$", None), timeout=2.5)
    require(response(master.port, "GET", binary_key) == (b"$", updated), "Rejected replica write changed master")
    replica.stop(kill=True)
    response(master.port, "SET", "replica_offline_resp", b"offline\x00\r\n")
    replica.start()
    eventually_resp(replica.port, ("GET", "replica_offline_resp"), (b"$", b"offline\x00\r\n"))
    eventually_resp(replica.port, ("GET", binary_key), (b"$", updated))
    print("PASS replica binary snapshot, asynchronous updates, readonly mode and restart")


def test_typescript_client(master, project):
    node = shutil.which("node")
    require(node is not None, "Node.js is required to test client.ts")
    client_url = (project / "client.ts").as_uri()
    script = f"""
import {{ VeloClient }} from {client_url!r};
const client = new VeloClient({master.port});
function expect(actual, expected) {{
  if (actual !== expected) throw new Error(`Expected ${{JSON.stringify(expected)}}, got ${{JSON.stringify(actual)}}`);
}}
expect(await client.ping(), 'PONG');
expect(await client.set('TS key with spaces\\r\\n\\0世界', 'Bună\\0\\r\\nVeloCache'), 'OK');
expect(await client.get('TS key with spaces\\r\\n\\0世界'), 'Bună\\0\\r\\nVeloCache');
expect(await client.set('TS empty', ''), 'OK');
expect(await client.get('TS empty'), '');
expect(await client.get('TS missing'), '(nil)');
expect(await client.set('TS ttl', 'temporary', 1), 'OK');
expect(await client.get('TS ttl'), 'temporary');
await new Promise(resolve => setTimeout(resolve, 1150));
expect(await client.get('TS ttl'), '(nil)');
let rejected = false;
try {{ await client.set('TS invalid', 'value', 0); }} catch {{ rejected = true; }}
if (!rejected) throw new Error('Invalid TTL was accepted');
console.log('PASS native TypeScript client with RESP, Unicode, binary strings and EX');
"""
    result = subprocess.run([node, "--input-type=module", "-e", script],
                            cwd=project, capture_output=True, text=True, timeout=10.0)
    require(result.returncode == 0, f"TypeScript client failed:\n{result.stdout}\n{result.stderr}")
    print(result.stdout.strip())


def main():
    require(len(sys.argv) == 2, "Usage: python3 tests/resp_integration.py /path/to/velocache")
    binary = pathlib.Path(sys.argv[1]).resolve()
    require(binary.is_file(), f"Binary does not exist: {binary}")
    project = pathlib.Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="velocache-resp-integration-") as temporary:
        directory = pathlib.Path(temporary)
        master_port, replica_port = free_ports(2)
        master = Server(binary, master_port, directory / "master.aof", directory / "master.log")
        replica = Server(binary, replica_port, directory / "replica.aof", directory / "replica.log", master_port)
        servers = [master, replica]
        try:
            master.start()
            binary_key, binary_value = test_connections(master)
            test_command_errors(master)
            test_protocol_errors(master)
            test_half_close(master)
            test_typescript_client(master, project)
            test_persistence(master, binary_key, binary_value)
            test_replication(master, replica, binary_key, binary_value)
        except Exception:
            for server in servers:
                if server.logs.exists():
                    print(f"LOG {server.logs.name}:\n{server.logs.read_text(errors='replace')[-4000:]}", file=sys.stderr)
            raise
        finally:
            for server in servers:
                server.stop()
    print("All RESP integration tests passed")


if __name__ == "__main__":
    main()
