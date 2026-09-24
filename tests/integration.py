import argparse
import concurrent.futures
import contextlib
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time


def frame(*args):
    args = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in args]
    body = struct.pack('=I', len(args))
    body += b''.join(struct.pack('=I', len(arg)) + arg for arg in args)
    return struct.pack('=I', len(body)) + body


def read_exact(sock, count):
    data = b''
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise EOFError('connection closed before response completed')
        data += part
    return data


def decode(data, offset=0):
    tag = data[offset]
    offset += 1
    if tag == 0:
        return None, offset
    if tag in (3, 4):
        return struct.unpack_from('=q' if tag == 3 else '=d', data, offset)[0], offset + 8
    if tag == 1:
        code = struct.unpack_from('=I', data, offset)[0]
        message, end = decode(bytes([2]) + data[offset + 4:])
        return ('error', code, message), offset + 4 + end - 1
    count = struct.unpack_from('=I', data, offset)[0]
    offset += 4
    if tag == 2:
        return data[offset:offset + count], offset + count
    assert tag == 5
    items = []
    for _ in range(count):
        item, offset = decode(data, offset)
        items.append(item)
    return items, offset


def response(sock):
    length = struct.unpack('=I', read_exact(sock, 4))[0]
    assert length <= 32 << 20
    data = read_exact(sock, length)
    value, end = decode(data)
    assert end == len(data)
    return value


def request(port, *args):
    with socket.create_connection(('127.0.0.1', port), timeout=5) as sock:
        sock.sendall(frame(*args))
        return response(sock)


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


@contextlib.contextmanager
def process(binary, port, *args):
    with tempfile.TemporaryFile() as log:
        child = subprocess.Popen([str(binary), '--port', str(port), *map(str, args)], stderr=log)
        try:
            deadline = time.monotonic() + 10
            while True:
                if child.poll() is not None:
                    log.seek(0)
                    raise AssertionError(log.read().decode())
                try:
                    if request(port, 'ping') == b'PONG':
                        break
                except (OSError, EOFError):
                    pass
                if time.monotonic() > deadline:
                    raise AssertionError('server did not become ready')
                time.sleep(0.02)
            yield child
        finally:
            if child.poll() is None:
                child.terminate()
            try:
                code = child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
                raise AssertionError('server did not shut down gracefully')
            log.seek(0)
            output = log.read().decode()
            assert code == 0, output
            assert 'Sanitizer' not in output and 'runtime error:' not in output, output


def exercise(port):
    assert request(port, 'get', 'missing') is None
    assert request(port, 'set', b'key\0', b'value\0') == b'OK'
    assert request(port, 'get', b'key\0') == b'value\0'
    assert request(port, 'incr', 'counter') == 1
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
        results = list(executor.map(lambda _: request(port, 'incr', 'counter'), range(80)))
    assert sorted(results) == list(range(2, 82))
    assert request(port, 'pexpire', 'counter', '10') == 1
    time.sleep(0.03)
    assert request(port, 'exists', 'counter') == 0
    assert request(port, 'pttl', 'counter') == -2
    assert request(port, 'zadd', 'rank', '10', 'alice') == 1
    assert request(port, 'zadd', 'rank', '5', 'bob') == 1
    assert request(port, 'zquery', 'rank', '-inf', '', 0, 4) == [b'bob', 5., b'alice', 10.]
    assert request(port, 'del', 'rank') == 1
    for i in range(30):
        assert request(port, 'set', f'key{i}', i) == b'OK'
    assert set(request(port, 'keys')) == {b'key\0', *(f'key{i}'.encode() for i in range(30))}
    assert request(port, 'keys', 'extra')[0] == 'error'
    with socket.create_connection(('127.0.0.1', port), timeout=5) as sock:
        sock.sendall(frame('ping') + frame('get', 'key0') + frame('exists', 'missing'))
        assert [response(sock) for _ in range(3)] == [b'PONG', b'0', 0]
    with socket.create_connection(('127.0.0.1', port), timeout=5) as sock:
        encoded = frame('get', 'key1')
        for byte in encoded:
            sock.sendall(bytes([byte]))
        assert response(sock) == b'1'
    for malformed in (struct.pack('=I', (32 << 20) + 1), struct.pack('=II', 4, 1), struct.pack('=I', 0)):
        with socket.create_connection(('127.0.0.1', port), timeout=5) as sock:
            sock.sendall(malformed)
            assert sock.recv(1) == b''
    assert request(port, 'ping') == b'PONG'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('bin'))
    binaries = parser.parse_args().bin_dir.resolve()
    port = free_port()
    with process(binaries / 'kv-server', port):
        exercise(port)
        client = [str(binaries / 'kv-client'), '-h', 'localhost', '-p', str(port)]
        assert subprocess.run([*client, 'set', 'option', '-p'], capture_output=True).returncode == 0
        assert request(port, 'get', 'option') == b'-p'
        assert subprocess.run([*client, 'bad-command'], capture_output=True).returncode == 1
        assert request(port, 'set', 'large', 'x' * 10000) == b'OK'
        result = subprocess.run([*client, 'get', 'large'], capture_output=True)
        assert result.returncode == 0 and b'x' * 10000 in result.stdout
    ports = [free_port() for _ in range(3)]
    with contextlib.ExitStack() as stack:
        shards = [stack.enter_context(process(binaries / 'kv-shard', p, '--id', i, '--shards', 2))
                  for i, p in enumerate(ports[:2])]
        stack.enter_context(process(binaries / 'kv-proxy', ports[2],
                                    '--shard', f'localhost:{ports[0]}', '--shard', f'localhost:{ports[1]}'))
        exercise(ports[2])
        shards[0].terminate()
        shards[0].wait(timeout=5)
        assert request(ports[2], 'keys')[0] == 'error'
        assert request(ports[2], 'ping') == b'PONG'
    for name in ('kv-server', 'kv-shard', 'kv-proxy', 'kv-client'):
        option = '-p' if name == 'kv-client' else '--port'
        for value in ('0', '-1', '65536', 'abc', '12x'):
            assert subprocess.run([str(binaries / name), option, value], capture_output=True).returncode == 1
    print('integration: single node, cluster, concurrency, TTL, protocol and CLI passed')


if __name__ == '__main__':
    main()
