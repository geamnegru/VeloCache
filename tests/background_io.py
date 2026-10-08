import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

from integration import HOST, Server, expect, free_ports, require
from resp_integration import RespConnection, encode_request


def wait_file(path):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.005)
    raise AssertionError("Injected fsync did not start")


def main():
    require(len(sys.argv) == 3, "Usage: background_io.py BINARY INTERPOSER")
    binary = pathlib.Path(sys.argv[1]).resolve()
    interposer = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="velocache-background-") as temporary:
        directory = pathlib.Path(temporary)
        marker = directory / "slow"
        started = directory / "started"
        failure = directory / "failure"
        environment = os.environ.copy()
        environment.update({
            "DYLD_INSERT_LIBRARIES": str(interposer),
            "VELOCACHE_TEST_FSYNC_MARKER": str(marker),
            "VELOCACHE_TEST_FSYNC_STARTED": str(started),
            "VELOCACHE_TEST_FSYNC_FAILURE": str(failure),
        })
        port = free_ports(1)[0]
        aof = directory / "master.aof"
        log = (directory / "server.log").open("wb")
        process = subprocess.Popen(
            [str(binary), "--bind", HOST, "--port", str(port), "--aof", str(aof)],
            stdout=log, stderr=log, env=environment,
        )
        try:
            deadline = time.monotonic() + 5
            while True:
                require(process.poll() is None, "Injected server exited at startup: " + (directory / "server.log").read_text(errors="replace"))
                try:
                    expect(port, "PING", "PONG")
                    break
                except OSError:
                    require(time.monotonic() < deadline, "Server did not start")
                    time.sleep(0.01)
            marker.touch()
            with RespConnection(port) as writer:
                writer.socket.sendall(encode_request("SET", "slow", "durable") +
                                      encode_request("GET", "slow"))
                wait_file(started)
                began = time.monotonic()
                expect(port, "PING", "PONG")
                require(time.monotonic() - began < 0.4, "Slow fsync blocked the event loop")
                writer.socket.settimeout(0.05)
                try:
                    early = writer.socket.recv(1)
                except socket.timeout:
                    early = None
                require(early is None, "SET replied before fsync completed")
                writer.socket.settimeout(3)
                require(writer.receive() == (b"+", b"OK"), "Durable write did not complete")
                require(writer.receive() == (b"$", b"durable"), "Pipeline read overtook write")
            marker.unlink()
            print("PASS slow fsync leaves PING responsive and delays OK until durability")

            started.unlink()
            marker.touch()
            failure.touch()
            with socket.create_connection((HOST, port), timeout=3) as writer:
                writer.settimeout(3)
                writer.sendall(encode_request("SET", "failure", "unacknowledged"))
                wait_file(started)
                received = bytearray()
                while True:
                    chunk = writer.recv(1024)
                    if not chunk:
                        break
                    received.extend(chunk)
                require(b"+OK" not in received, "Failed fsync was acknowledged")
            require(process.wait(timeout=3) != 0, "Failed fsync did not stop the server")
            print("PASS failed background fsync stops server without acknowledging the write")
        finally:
            marker.unlink(missing_ok=True)
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            log.close()
        recovered = Server(binary, port, aof, directory / "recovered.log")
        try:
            recovered.start()
            expect(port, "GET slow", "durable")
        finally:
            recovered.stop()
        print("PASS background-acknowledged write survives restart")


if __name__ == "__main__":
    main()
