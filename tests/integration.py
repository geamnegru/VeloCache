import pathlib
import queue
import resource
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time


HOST = "127.0.0.1"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def free_ports(count):
    sockets = []
    try:
        for _ in range(count):
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.bind((HOST, 0))
            sockets.append(listener)
        return [listener.getsockname()[1] for listener in sockets]
    finally:
        for listener in sockets:
            listener.close()


def read_response(connection):
    response = bytearray()
    while b"\n" not in response:
        chunk = connection.recv(65536)
        require(bool(chunk), "Connection closed before a response")
        response.extend(chunk)
    require(response.endswith(b"\n"), "Response has trailing protocol data")
    require(connection.recv(1) == b"", "Connection remained open after response")
    return response[:-1].decode("utf-8")


def request(port, command, timeout=2.0):
    with socket.create_connection((HOST, port), timeout=timeout) as connection:
        connection.settimeout(timeout)
        connection.sendall((command + "\n").encode("utf-8"))
        return read_response(connection)


def expect(port, command, expected):
    actual = request(port, command)
    require(actual == expected, f"{command!r}: expected {expected!r}, got {actual!r}")


def eventually(port, command, expected, timeout=5.0):
    deadline = time.monotonic() + timeout
    actual = None
    while time.monotonic() < deadline:
        try:
            actual = request(port, command, timeout=0.5)
            if actual == expected:
                return
        except (OSError, AssertionError) as error:
            actual = str(error)
        time.sleep(0.05)
    raise AssertionError(f"{command!r}: expected {expected!r}, last result {actual!r}")


def eventually_error(port, command, timeout=3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        actual = request(port, command)
        if actual.startswith("ERR"):
            return
        time.sleep(0.025)
    raise AssertionError(f"{command!r}: replica continued serving data during synchronization")


def encode_mutation(sequence, key, value, expires_at_ms=0):
    key_hex = key.encode("utf-8").hex() or "-"
    value_hex = value.encode("utf-8").hex() or "-"
    payload = f"UPDATE {sequence} {expires_at_ms} {key_hex} {value_hex}".encode("ascii")
    checksum = 14695981039346656037
    for byte in payload:
        checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return payload + f" {checksum:016x}\n".encode("ascii")


def encode_snapshot(sequence, entries):
    result = f"SNAP_BEGIN {sequence} {len(entries)}\n".encode("ascii")
    for key, value in entries:
        result += encode_mutation(sequence, key, value)
    return result + f"SNAP_END {sequence}\n".encode("ascii")


def send_fragmented(connection, data):
    for offset in range(0, len(data), 7):
        connection.sendall(data[offset:offset + 7])
        time.sleep(0.002)


class FakeMaster:
    def __init__(self):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind((HOST, 0))
        self.listener.listen(4)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.accepted = queue.Queue()
        self.connections = []
        self.errors = []
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        while not self.stopped.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError as error:
                if not self.stopped.is_set():
                    self.errors.append(error)
                return
            self.connections.append(connection)
            try:
                connection.settimeout(2.0)
                handshake = bytearray()
                while b"\n" not in handshake:
                    chunk = connection.recv(1024)
                    require(bool(chunk), "Replica disconnected before SYNC")
                    handshake.extend(chunk)
                require(handshake == b"SYNC\n", f"Unexpected replica handshake: {handshake!r}")
                self.accepted.put(connection)
            except Exception as error:
                self.errors.append(error)
                connection.close()

    def next_connection(self):
        try:
            return self.accepted.get(timeout=5.0)
        except queue.Empty:
            raise AssertionError(f"Replica did not reconnect to fake master: {self.errors}")

    def close(self):
        self.stopped.set()
        self.listener.close()
        for connection in self.connections:
            connection.close()
        self.thread.join(timeout=2.0)
        require(not self.thread.is_alive(), "Fake master thread did not stop")


class Server:
    def __init__(self, binary, port, aof, logs, upstream=None, nofile_limit=None):
        self.binary = binary
        self.port = port
        self.aof = aof
        self.logs = logs
        self.upstream = upstream
        self.nofile_limit = nofile_limit
        self.process = None
        self.log_file = None

    def launch(self):
        args = [
            str(self.binary), "--port", str(self.port),
            "--aof", str(self.aof), "--bind", HOST,
        ]
        if self.upstream is not None:
            args.extend(["--replicaof", HOST, str(self.upstream)])
        self.log_file = self.logs.open("ab")

        def restrict_descriptors():
            resource.setrlimit(resource.RLIMIT_NOFILE, (self.nofile_limit, self.nofile_limit))

        self.process = subprocess.Popen(
            args, stdout=self.log_file, stderr=self.log_file,
            preexec_fn=restrict_descriptors if self.nofile_limit is not None else None,
        )

    def start(self):
        self.launch()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            require(self.process.poll() is None, f"Server failed to start: {self.logs.read_text(errors='replace')}")
            try:
                if request(self.port, "PING", timeout=0.2) == "PONG":
                    return
            except (OSError, AssertionError):
                pass
            time.sleep(0.025)
        raise AssertionError(f"Server on port {self.port} did not start")

    def stop(self, kill=False):
        if self.process is not None and self.process.poll() is None:
            if kill:
                self.process.kill()
            else:
                self.process.terminate()
            try:
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2.0)
        if self.log_file is not None:
            self.log_file.close()
            self.log_file = None


def test_descriptor_exhaustion(binary, directory, servers):
    limited = Server(
        binary, free_ports(1)[0], directory / "limited.aof",
        directory / "limited.log", nofile_limit=64,
    )
    servers.append(limited)
    clients = []
    try:
        limited.start()
        for _ in range(80):
            clients.append(socket.create_connection((HOST, limited.port), timeout=2.0))
        time.sleep(0.3)
        require(limited.process.poll() is None, "Descriptor exhaustion terminated the server")
        for connection in clients:
            connection.close()
        clients.clear()
        eventually(limited.port, "PING", "PONG")
        require(limited.process.poll() is None, "Server failed to recover after descriptor exhaustion")
        print("PASS descriptor exhaustion at RLIMIT_NOFILE=64 and listener recovery")
    finally:
        for connection in clients:
            connection.close()
        limited.stop()


def test_clients(master):
    port = master.port
    expect(port, "PING", "PONG")
    expect(port, "GET missing", "(nil)")
    expect(port, "SET greeting Salut VeloCache", "OK")
    expect(port, "GET greeting", "Salut VeloCache")
    expect(port, "SET empty ", "OK")
    expect(port, "GET empty", "")
    expect(port, "SET spaces valoare  cu   spatii", "OK")
    expect(port, "GET spaces", "valoare  cu   spatii")
    with socket.create_connection((HOST, port), timeout=2.0) as connection:
        connection.settimeout(2.0)
        for part in [b"SE", b"T fragment", b"ed valo", b"are impartita\r", b"\n"]:
            connection.sendall(part)
            time.sleep(0.01)
        require(read_response(connection) == "OK", "Fragmented command failed")
    expect(port, "GET fragmented", "valoare impartita")
    expect(port, "SET invalid unchanged", "OK")
    for ttl in ["0", "-1", "nope", "1x", "99999999999999999999999999"]:
        response = request(port, "SET invalid modified EX " + ttl)
        require(response.startswith("ERR"), f"Invalid TTL accepted: {ttl!r}")
        expect(port, "GET invalid", "unchanged")
    expect(port, "SET short lived EX 1", "OK")
    expect(port, "SET reset before EX 1", "OK")
    expect(port, "SET reset forever", "OK")
    time.sleep(1.15)
    expect(port, "GET short", "(nil)")
    expect(port, "GET reset", "forever")
    slow_clients = []
    try:
        for _ in range(120):
            connection = socket.create_connection((HOST, port), timeout=2.0)
            connection.sendall(b"GET missing")
            slow_clients.append(connection)
        started = time.monotonic()
        expect(port, "PING", "PONG")
        require(time.monotonic() - started < 2.0, "Slow clients blocked the event loop")
        for connection in slow_clients:
            connection.settimeout(2.0)
            connection.sendall(b"\n")
            require(read_response(connection) == "(nil)", "Partial client command was lost")
    finally:
        for connection in slow_clients:
            connection.close()
    print("PASS clients, TTL validation, expiry and 120 partial connections")


def test_persistence(master):
    expect(master.port, "SET durable persisted after SIGKILL", "OK")
    expect(master.port, "SET restart_ttl expires on original deadline EX 2", "OK")
    deadline = time.monotonic() + 2.0
    master.stop(kill=True)
    time.sleep(0.3)
    master.start()
    expect(master.port, "GET durable", "persisted after SIGKILL")
    expect(master.port, "GET empty", "")
    expect(master.port, "GET reset", "forever")
    expect(master.port, "GET restart_ttl", "expires on original deadline")
    time.sleep(max(0.0, deadline - time.monotonic() + 0.15))
    expect(master.port, "GET restart_ttl", "(nil)")
    master.stop(kill=True)
    size = master.aof.stat().st_size
    with master.aof.open("ab") as journal:
        journal.write(b"UPDATE 999 0 616263")
    master.start()
    expect(master.port, "GET durable", "persisted after SIGKILL")
    require(master.aof.stat().st_size == size, "Incomplete AOF tail was not truncated")
    print("PASS SIGKILL recovery, absolute TTL and incomplete AOF tail repair")


def test_replication(master, replica):
    expect(master.port, "SET initial snapshot value", "OK")
    expect(master.port, "SET replica_ttl transient EX 2", "OK")
    replica.start()
    eventually(replica.port, "GET initial", "snapshot value")
    eventually(replica.port, "GET replica_ttl", "transient", timeout=1.0)
    eventually(replica.port, "GET empty", "")
    eventually(replica.port, "GET spaces", "valoare  cu   spatii")
    response = request(replica.port, "SET initial illegal")
    require(response.startswith("ERR"), "Replica accepted a write")
    expect(master.port, "GET initial", "snapshot value")
    for index in range(120):
        expect(master.port, f"SET item_{index} value_{index}", "OK")
        expect(master.port, f"SET latest {index}", "OK")
    eventually(replica.port, "GET latest", "119")
    for index in range(120):
        expect(replica.port, f"GET item_{index}", f"value_{index}")
    eventually(master.port, "GET replica_ttl", "(nil)", timeout=3.0)
    eventually(replica.port, "GET replica_ttl", "(nil)", timeout=3.0)
    master.stop(kill=True)
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        if request(replica.port, "GET initial").startswith("ERR"):
            break
        time.sleep(0.025)
    else:
        raise AssertionError("Disconnected replica continued to serve stale values")
    expect(replica.port, "PING", "PONG")
    master.start()
    expect(master.port, "SET after_restart reconnected", "OK")
    eventually(replica.port, "GET after_restart", "reconnected")
    expect(replica.port, "GET latest", "119")
    replica.stop(kill=True)
    expect(master.port, "SET offline written while replica down", "OK")
    replica.start()
    eventually(replica.port, "GET offline", "written while replica down")
    print("PASS replica snapshot, ordered updates, read-only mode and reconnection")


def test_corruption(binary, master, directory, servers):
    master.stop(kill=True)
    corrupt_aof = directory / "corrupt.aof"
    shutil.copyfile(master.aof, corrupt_aof)
    data = bytearray(corrupt_aof.read_bytes())
    record = data.find(b"UPDATE ")
    require(record >= 0, "No UPDATE record available for corruption test")
    position = data.find(b"\n", record) - 1
    require(position >= 0, "No complete AOF record available for corruption test")
    data[position] = ord("0") if data[position] != ord("0") else ord("1")
    corrupt_aof.write_bytes(data)
    broken = Server(binary, free_ports(1)[0], corrupt_aof, directory / "corrupt.log")
    servers.append(broken)
    broken.launch()
    try:
        code = broken.process.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        raise AssertionError("Server accepted a complete AOF record with a corrupt checksum")
    require(code != 0, "Corrupt AOF exited successfully")
    print("PASS complete AOF checksum corruption rejected")


def test_incomplete_replication(binary, replica, directory, servers):
    aof = directory / "interrupted-replica.aof"
    shutil.copyfile(replica.aof, aof)
    before_snapshot = aof.read_bytes()
    fake = FakeMaster()
    interrupted = Server(binary, free_ports(1)[0], aof, directory / "interrupted-replica.log", fake.port)
    servers.append(interrupted)
    fake.thread.start()
    try:
        interrupted.start()
        connection = fake.next_connection()
        connection.sendall(b"SNAP_BEGIN 100 1\n" + encode_mutation(100, "partial", "must stay hidden"))
        eventually_error(interrupted.port, "GET partial")
        require(aof.read_bytes() == before_snapshot, "Incomplete snapshot changed AOF")
        connection.shutdown(socket.SHUT_RDWR)
        connection.close()
        connection = fake.next_connection()
        require(aof.read_bytes() == before_snapshot, "Disconnect committed an incomplete snapshot")
        send_fragmented(connection, encode_snapshot(200, [("complete", "fully synchronized"), ("empty", "")]))
        eventually(interrupted.port, "GET complete", "fully synchronized")
        expect(interrupted.port, "GET empty", "")
        expect(interrupted.port, "GET partial", "(nil)")
        expect(interrupted.port, "GET offline", "(nil)")
        before_gap = aof.read_bytes()
        connection.sendall(encode_mutation(202, "bad_gap", "must not appear"))
        require(connection.recv(1) == b"", "Replica retained upstream after a sequence gap")
        eventually_error(interrupted.port, "GET bad_gap")
        require(aof.read_bytes() == before_gap, "Sequence gap changed AOF")
        connection = fake.next_connection()
        send_fragmented(connection, encode_snapshot(300, [("after_gap", "resynchronized")]))
        eventually(interrupted.port, "GET after_gap", "resynchronized")
        expect(interrupted.port, "GET bad_gap", "(nil)")
        before_checksum = aof.read_bytes()
        invalid = bytearray(encode_mutation(301, "bad_checksum", "must not appear"))
        invalid[-2] = ord("0") if invalid[-2] != ord("0") else ord("1")
        connection.sendall(invalid)
        require(connection.recv(1) == b"", "Replica retained upstream after checksum corruption")
        eventually_error(interrupted.port, "GET bad_checksum")
        require(aof.read_bytes() == before_checksum, "Invalid checksum changed AOF")
        connection = fake.next_connection()
        send_fragmented(connection, encode_snapshot(400, [("after_checksum", "resynchronized again")]))
        eventually(interrupted.port, "GET after_checksum", "resynchronized again")
        expect(interrupted.port, "GET bad_checksum", "(nil)")
        require(not fake.errors, f"Fake master failed: {fake.errors}")
        print("PASS incomplete snapshot, fragmented frames, invalid updates and resynchronization")
    finally:
        interrupted.stop()
        fake.close()


def main():
    require(len(sys.argv) == 2, "Usage: python3 tests/integration.py /path/to/velocache")
    binary = pathlib.Path(sys.argv[1]).resolve()
    require(binary.is_file(), f"Binary does not exist: {binary}")
    with tempfile.TemporaryDirectory(prefix="velocache-integration-") as temp:
        directory = pathlib.Path(temp)
        master_port, replica_port = free_ports(2)
        master = Server(binary, master_port, directory / "master.aof", directory / "master.log")
        replica = Server(binary, replica_port, directory / "replica.aof", directory / "replica.log", master_port)
        servers = [master, replica]
        try:
            test_descriptor_exhaustion(binary, directory, servers)
            master.start()
            test_clients(master)
            test_persistence(master)
            test_replication(master, replica)
            replica.stop()
            test_incomplete_replication(binary, replica, directory, servers)
            test_corruption(binary, master, directory, servers)
        except Exception:
            for server in servers:
                if server.logs.exists():
                    print(f"LOG {server.logs.name}:\n{server.logs.read_text(errors='replace')[-4000:]}", file=sys.stderr)
            raise
        finally:
            for server in servers:
                server.stop()
    print("All integration tests passed")


if __name__ == "__main__":
    main()
