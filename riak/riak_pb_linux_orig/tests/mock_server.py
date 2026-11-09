#!/usr/bin/env python3
"""Run the native extension against a local, independent Riak PB test peer."""

import argparse
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path


def varint(value):
    encoded = bytearray()
    while value > 0x7f:
        encoded.append((value & 0x7f) | 0x80)
        value >>= 7
    encoded.append(value)
    return bytes(encoded)


def integer(number, value):
    return varint(number << 3) + varint(value)


def blob(number, value):
    return varint((number << 3) | 2) + varint(len(value)) + value


def read_varint(data, position):
    result = 0
    for shift in range(0, 70, 7):
        if position >= len(data):
            raise AssertionError("truncated request varint")
        byte = data[position]
        position += 1
        if shift == 63 and byte > 1:
            raise AssertionError("request varint overflow")
        result |= (byte & 0x7f) << shift
        if byte < 128:
            return result, position
    raise AssertionError("request varint too long")


def fields(data):
    result = {}
    position = 0
    while position < len(data):
        tag, position = read_varint(data, position)
        number, wire = tag >> 3, tag & 7
        if number == 0:
            raise AssertionError("request field zero")
        if wire == 0:
            value, position = read_varint(data, position)
        elif wire == 2:
            length, position = read_varint(data, position)
            if length > len(data) - position:
                raise AssertionError("truncated request field")
            value = data[position:position + length]
            position += length
        else:
            raise AssertionError("unexpected request wire type")
        if number in result:
            raise AssertionError("duplicate request field")
        result[number] = value
    return result


def receive_exact(connection, length):
    result = bytearray()
    while len(result) < length:
        chunk = connection.recv(min(length - len(result), 65536))
        if not chunk:
            if not result:
                return None
            raise AssertionError("truncated client frame")
        result.extend(chunk)
    return bytes(result)


def content(value, mime=b"application/octet-stream", deleted=False):
    result = blob(1, value) + blob(2, mime)
    if deleted:
        result += integer(11, 1)
    return result


class MockRiak(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self):
        self.objects = {}
        self.buckets = {}
        self.failures = []
        self.operations = set()
        self.lock = threading.Lock()
        super().__init__(("127.0.0.1", 0), RiakHandler)

    def record_failure(self, message):
        with self.lock:
            self.failures.append(message)


class RiakHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(3)
        self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            while True:
                header = receive_exact(self.request, 4)
                if header is None:
                    return
                length = struct.unpack(">I", header)[0]
                if not 1 <= length <= 2 * 1024 * 1024:
                    raise AssertionError("invalid client frame length")
                request = receive_exact(self.request, length)
                if request is None:
                    raise AssertionError("missing client frame body")
                if not self.dispatch(request[0], request[1:]):
                    return
        except (BrokenPipeError, ConnectionResetError):
            # Deliberately late or malformed responses can be abandoned by PHP.
            pass
        except Exception as error:
            self.server.record_failure(str(error))

    def send_frame(self, code, payload=b""):
        frame = struct.pack(">I", 1 + len(payload)) + bytes([code]) + payload
        # Split both the four-byte header and the protobuf body across writes.
        for index in range(min(len(frame), 8)):
            self.request.sendall(frame[index:index + 1])
            time.sleep(0.001)
        for index in range(8, len(frame), 2048):
            self.request.sendall(frame[index:index + 2048])

    def dispatch(self, code, payload):
        request = fields(payload)
        with self.server.lock:
            self.server.operations.add(code)
        if code == 1:
            assert request == {}, "ping must have no protobuf payload"
            self.send_frame(2)
        elif code == 7:
            assert request == {}, "server info must have no protobuf payload"
            self.send_frame(8, blob(1, b"riak@mock") + blob(2, b"2.0.0"))
        elif code == 21:
            assert request[1] == b"test", "wrong bucket in SetBucket"
            properties = fields(request[2])
            assert properties == {2: 0}, "allow_mult=false must be explicitly encoded"
            with self.server.lock:
                self.server.buckets[request[1]] = properties[2]
            self.send_frame(22)
        elif code == 11:
            assert request[1] == b"test", "wrong bucket in Put"
            assert isinstance(request[2], bytes) and request[2], "missing Put key"
            assert request[7] == 0, "Put must explicitly request no returned body"
            assert 1 <= request[12] <= 600000, "wrong Put timeout field"
            value = fields(request[4])
            assert isinstance(value[1], bytes), "Put content.value is required"
            assert isinstance(value[2], bytes), "Put content_type is required"
            with self.server.lock:
                self.server.objects[(request[1], request[2])] = (value[1], value[2])
            self.send_frame(12)  # return_body=false permits an empty PutResp.
        elif code == 13:
            assert request[1] == b"test", "wrong bucket in Del"
            assert 1 <= request[10] <= 600000, "wrong Del timeout field"
            with self.server.lock:
                self.server.objects.pop((request[1], request[2]), None)
            self.send_frame(14)
        elif code == 9:
            assert request[1] == b"test", "wrong bucket in Get"
            assert 1 <= request[10] <= 600000, "wrong Get timeout field"
            return self.get(request[1], request[2])
        else:
            raise AssertionError("unexpected request message code %s" % code)
        return True

    def get(self, bucket, key):
        if key == b"server-error":
            self.send_frame(0, blob(1, b"bucket unavailable") + integer(2, 99))
        elif key == b"error-without-message":
            self.send_frame(0, integer(2, 99))
        elif key == b"error-without-code":
            self.send_frame(0, blob(1, b"missing error code"))
        elif key == b"error-code-overflow":
            self.send_frame(0, blob(1, b"too large") + integer(2, 0x100000000))
        elif key == b"error-wrong-wire":
            self.send_frame(0, integer(1, 4) + integer(2, 99))
        elif key == b"error-truncated":
            self.send_frame(0, blob(1, b"truncated") + b"\x10\x80")
        elif key == b"wrong-code":
            self.send_frame(12)
        elif key == b"malformed":
            self.send_frame(10, b"\x0a\x05abc")
        elif key == b"malformed-content":
            self.send_frame(10, blob(1, b"\x0a\x05abc"))
        elif key == b"missing-value":
            self.send_frame(10, blob(1, blob(2, b"text/plain")))
        elif key == b"wrong-value-wire":
            self.send_frame(10, blob(1, integer(1, 42)))
        elif key == b"oversize":
            self.request.sendall(struct.pack(">I", 0x7fffffff))
            return False
        elif key == b"zero-frame":
            self.request.sendall(struct.pack(">I", 0))
            return False
        elif key == b"closed":
            return False
        elif key == b"truncated-frame":
            self.request.sendall(struct.pack(">I", 20) + b"\x0a\x0a\x05ab")
            return False
        elif key == b"timeout":
            time.sleep(0.5)
            self.send_frame(10)
        elif key == b"slow-drip":
            payload = blob(1, content(b"slow response"))
            frame = struct.pack(">I", len(payload) + 1) + b"\x0a" + payload
            for byte in frame:
                self.request.sendall(bytes([byte]))
                time.sleep(0.025)
        elif key == b"siblings":
            self.send_frame(10, blob(1, content(b"first")) + blob(1, content(b"second")))
        elif key == b"deleted":
            self.send_frame(10, blob(1, content(b"", deleted=True)))
        elif key == b"unknown-fields":
            unknown = integer(80, 123) + varint((81 << 3) | 5) + b"abcd"
            unknown += varint((82 << 3) | 1) + b"abcdefgh"
            self.send_frame(10, unknown + blob(1, content(b"known", b"text/plain")))
        elif key == b"default-mime":
            self.send_frame(10, blob(1, blob(1, b"raw")))
        else:
            with self.server.lock:
                value = self.server.objects.get((bucket, key))
            self.send_frame(10, b"" if value is None else blob(1, content(*value)))
        return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--php", default="php", help="PHP 8.4 executable")
    parser.add_argument("--extension", required=True, help="compiled riak_pb.so or DLL")
    args = parser.parse_args()
    extension = str(Path(args.extension).resolve())
    if not Path(extension).is_file():
        parser.error("compiled extension does not exist: " + extension)

    with MockRiak() as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        print("READY 127.0.0.1:%s" % server.server_address[1], flush=True)
        try:
            command = [args.php, "-n", "-d", "extension=" + extension,
                       str(Path(__file__).with_name("integration.php")),
                       str(server.server_address[1])]
            completed = subprocess.run(command, check=False, timeout=30)
            result = completed.returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            print("FAIL: " + str(error), file=sys.stderr)
            result = 1
        finally:
            server.shutdown()
            worker.join(timeout=3)
        for failure in server.failures:
            print("MOCK FAILURE: " + failure, file=sys.stderr)
        if server.failures:
            result = 1
        if result == 0 and server.operations != {1, 7, 9, 11, 13, 21}:
            print("MOCK FAILURE: not all operations were exercised", file=sys.stderr)
            result = 1
        return result


if __name__ == "__main__":
    sys.exit(main())
