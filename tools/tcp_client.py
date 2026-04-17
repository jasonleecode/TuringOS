#!/usr/bin/env python3
"""
TuringOS TCP client test script.

Connects to the tcp_server running inside QEMU and performs a simple
echo test. QEMU forwards host port 5555 → guest port 5000.

Usage:
    python3 tools/tcp_client.py [--host HOST] [--port PORT] [--rounds N]

Defaults:
    --host  127.0.0.1   (QEMU host-forward address)
    --port  5555        (QEMU hostfwd port; guest listens on 5000)
    --rounds 5          (number of echo round-trips)

QEMU must be started with:
    -netdev user,id=net0,hostfwd=tcp::5555-:5000
    -device virtio-net-device,netdev=net0
"""

import argparse
import socket
import sys
import time


BANNER_TIMEOUT = 5.0   # seconds to wait for the server banner
ECHO_TIMEOUT   = 3.0   # seconds to wait for echo reply


def recv_line(sock: socket.socket, timeout: float) -> str:
    """Read bytes until \\n or timeout."""
    sock.settimeout(timeout)
    buf = b""
    try:
        while b"\n" not in buf:
            chunk = sock.recv(256)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    return buf.decode("utf-8", errors="replace").strip()


def recv_exactly(sock: socket.socket, length: int, timeout: float) -> bytes:
    """Receive exactly `length` bytes within `timeout` seconds."""
    sock.settimeout(timeout)
    buf = b""
    try:
        while len(buf) < length:
            chunk = sock.recv(length - len(buf))
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    return buf


def run_test(host: str, port: int, rounds: int) -> bool:
    print(f"Connecting to {host}:{port} …", flush=True)

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
    except ConnectionRefusedError:
        print(f"ERROR: connection refused – is QEMU running with hostfwd=tcp::{port}-:5000?")
        return False
    except OSError as e:
        print(f"ERROR: {e}")
        return False

    print("Connected.\n")

    # Receive server banner
    banner = recv_line(sock, BANNER_TIMEOUT)
    if banner:
        print(f"Server banner: {banner!r}")
    else:
        print("WARNING: no banner received (server may not be ready yet)")

    # Echo rounds
    passed = 0
    failed = 0

    for i in range(1, rounds + 1):
        msg = f"Hello TuringOS, round {i}\r\n"
        payload = msg.encode()

        try:
            sock.settimeout(ECHO_TIMEOUT)
            sent = sock.send(payload)
        except OSError as e:
            print(f"  Round {i}: SEND ERROR – {e}")
            failed += 1
            continue

        echo = recv_exactly(sock, sent, ECHO_TIMEOUT)

        if echo == payload:
            rtt_ms = 0.0
            print(f"  Round {i}: OK  – echoed {sent} bytes")
            passed += 1
        else:
            print(f"  Round {i}: FAIL – sent {payload!r}, got {echo!r}")
            failed += 1

        time.sleep(0.01)

    sock.close()

    print(f"\nResult: {passed}/{rounds} rounds passed, {failed} failed")
    return failed == 0


def main() -> None:
    parser = argparse.ArgumentParser(
        description="TCP echo client for TuringOS tcp_server")
    parser.add_argument("--host",   default="127.0.0.1",
                        help="Server hostname (default: 127.0.0.1)")
    parser.add_argument("--port",   type=int, default=5555,
                        help="Host-side port forwarded to QEMU guest (default: 5555)")
    parser.add_argument("--rounds", type=int, default=5,
                        help="Number of echo rounds (default: 5)")
    args = parser.parse_args()

    ok = run_test(args.host, args.port, args.rounds)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
