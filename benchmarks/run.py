import argparse
import datetime
import json
import pathlib
import platform
import socket
import subprocess
import tempfile
import time


def request(stream, connection, *args):
    fields = [arg.encode() for arg in args]
    connection.sendall(b'*' + str(len(fields)).encode() + b'\r\n' + b''.join(
        b'$' + str(len(field)).encode() + b'\r\n' + field + b'\r\n' for field in fields))
    header = stream.readline()
    if header.startswith(b'$'):
        size = int(header[1:])
        if size < 0:
            return None
        value = stream.read(size)
        if stream.read(2) != b'\r\n':
            raise RuntimeError('Incomplete response')
        return value
    if not header.startswith(b'+'):
        raise RuntimeError(repr(header))
    return header[1:-2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default='./velocache')
    parser.add_argument('--requests', type=int, default=2000)
    parser.add_argument('--output', type=pathlib.Path)
    args = parser.parse_args()
    if args.requests < 1:
        parser.error('--requests must be positive')
    binary = str(pathlib.Path(args.binary).resolve())
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='velocache-benchmark-') as directory:
        with open(pathlib.Path(directory) / 'server.log', 'w+') as log:
            process = subprocess.Popen([binary, '--bind', '127.0.0.1', '--port', str(port),
                                        '--aof', str(pathlib.Path(directory) / 'bench.aof')], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 10
                while True:
                    try:
                        connection = socket.create_connection(('127.0.0.1', port), timeout=10)
                        break
                    except OSError:
                        if process.poll() is not None or time.monotonic() > deadline:
                            log.seek(0)
                            raise RuntimeError(log.read())
                        time.sleep(0.02)
                with connection, connection.makefile('rb') as stream:
                    value = 'x' * 64
                    assert request(stream, connection, 'SET', 'benchmark', value) == b'OK'
                    results = []
                    for name, command, expected in [('PING', ('PING',), b'PONG'),
                                                    ('GET', ('GET', 'benchmark'), value.encode()),
                                                    ('SET', ('SET', 'benchmark', value), b'OK')]:
                        for _ in range(50):
                            assert request(stream, connection, *command) == expected
                        samples = []
                        started = time.perf_counter()
                        for _ in range(args.requests):
                            tick = time.perf_counter()
                            actual = request(stream, connection, *command)
                            samples.append((time.perf_counter() - tick) * 1000)
                            if actual != expected:
                                raise RuntimeError(f'{name}: unexpected response {actual!r}')
                        elapsed = time.perf_counter() - started
                        samples.sort()
                        results.append({'command': name, 'requests': args.requests,
                                        'ops_per_second': round(args.requests / elapsed, 2),
                                        'p50_ms': round(samples[int((len(samples) - 1) * .50)], 4),
                                        'p95_ms': round(samples[int((len(samples) - 1) * .95)], 4),
                                        'p99_ms': round(samples[int((len(samples) - 1) * .99)], 4)})
                    report = {'timestamp_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                              'platform': platform.platform(), 'python': platform.python_version(),
                              'method': 'Loopback, one persistent connection, no pipelining, 64-byte value, 50 warmup requests per command, AOF enabled, SET acknowledged after fsync. Python client and server share the machine.',
                              'results': results}
                    rendered = json.dumps(report, indent=2) + '\n'
                    print(rendered, end='')
                    if args.output:
                        args.output.write_text(rendered)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == '__main__':
    main()
