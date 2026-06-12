#!/usr/bin/env python3
"""Verify the standalone mqtt tool (MQTT over netd) — pub + sub (plaintext).

Runs a minimal MQTT broker stub on 127.0.0.1:1883 (reachable from the guest as
10.0.2.2:1883 via slirp).  Then:
  run rom/mqtt pub 10.0.2.2 turingos/test hello_mqtt  -> broker receives it
  run rom/mqtt sub 10.0.2.2 turingos/test 8           -> broker pushes a msg
"""
import os, sys, socket, threading, time
import pexpect

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.path.join(PROJ, "build/l4re_virt/images/bootstrap_native-shell.elf")
DISK = os.path.join(PROJ, "build/virt_disk.img")

published = []          # (topic, payload) received by the broker

def enc_rl(n):
    out = bytearray()
    while True:
        b = n % 128; n //= 128
        if n: b |= 0x80
        out.append(b)
        if not n: break
    return bytes(out)

def recv_exact(c, n):
    d = b""
    while len(d) < n:
        ch = c.recv(n - len(d))
        if not ch: return None
        d += ch
    return d

def read_rl(c):
    mult, val = 1, 0
    while True:
        b = recv_exact(c, 1)
        if b is None: return None
        val += (b[0] & 0x7f) * mult; mult *= 128
        if not (b[0] & 0x80): return val

def broker_conn(c):
    try:
        while True:
            hdr = recv_exact(c, 1)
            if not hdr: break
            t = hdr[0] >> 4
            rl = read_rl(c)
            if rl is None: break
            body = recv_exact(c, rl) if rl else b""
            if t == 1:        # CONNECT
                c.sendall(bytes([0x20, 0x02, 0x00, 0x00]))
            elif t == 3:      # PUBLISH
                tl = (body[0] << 8) | body[1]
                topic = body[2:2+tl].decode(errors="replace")
                payload = body[2+tl:].decode(errors="replace")
                published.append((topic, payload))
            elif t == 8:      # SUBSCRIBE
                pid = body[0:2]
                c.sendall(bytes([0x90, 0x03]) + pid + bytes([0x00]))   # SUBACK
                topic, payload = b"turingos/test", b"from_broker"
                pkt = (bytes([0x30]) + enc_rl(2 + len(topic) + len(payload))
                       + bytes([len(topic) >> 8, len(topic) & 0xff]) + topic + payload)
                c.sendall(pkt)
            elif t == 12:     # PINGREQ
                c.sendall(bytes([0xd0, 0x00]))
            elif t == 14:     # DISCONNECT
                break
    except Exception:
        pass
    finally:
        c.close()

def broker(ready):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 1883)); s.listen(8); ready.set(); s.settimeout(90)
    try:
        while True:
            c, _ = s.accept()
            threading.Thread(target=broker_conn, args=(c,), daemon=True).start()
    except Exception:
        pass

ready = threading.Event()
threading.Thread(target=broker, args=(ready,), daemon=True).start()
ready.wait(5)

net = ("-netdev user,id=net0 "
       "-device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0")
disk = (f"-drive if=none,id=vdisk,file={DISK},format=raw "
        "-device virtio-blk-device,drive=vdisk,bus=virtio-mmio-bus.1")
cmd = (f"qemu-system-arm -M virt -cpu cortex-a15 -m 48M -smp 2 "
       f"-kernel {IMAGE} -display none -serial stdio -monitor none {net} {disk}")

child = pexpect.spawn(cmd, encoding="utf-8", timeout=120)
child.logfile = sys.stdout
rc = 0
fails = []
try:
    child.expect(r"\[netd\] stack ready", timeout=90)
    child.expect(r"login:", timeout=60)
    child.sendline("root"); child.expect(r"[Pp]assword:", timeout=10)
    child.sendline("12345678"); child.expect(r"turingos>", timeout=20)

    child.sendline("run rom/mqtt pub 10.0.2.2 turingos/test hello_mqtt")
    child.expect(r"published to 'turingos/test'", timeout=25)
    child.expect(r"turingos>", timeout=10)
    time.sleep(1)
    if ("turingos/test", "hello_mqtt") in published:
        print("\n[check] mqtt pub OK (broker received it)")
    else:
        fails.append(f"pub (broker got {published})")

    child.sendline("run rom/mqtt sub 10.0.2.2 turingos/test 8")
    try:
        child.expect(r"mqtt: \[turingos/test\] from_broker", timeout=30)
        print("\n[check] mqtt sub OK (received broker push)")
    except pexpect.TIMEOUT:
        fails.append("sub")

    if fails:
        print(f"\n\n=== RESULT: FAIL — {fails} ==="); rc = 1
    else:
        print("\n\n=== RESULT: PASS — mqtt pub + sub via netd ===")
except pexpect.TIMEOUT:
    print("\n\n=== RESULT: FAIL — timeout ==="); rc = 3
except pexpect.EOF:
    print("\n\n=== RESULT: FAIL — QEMU exited early ==="); rc = 4
finally:
    try: child.sendcontrol('a'); child.send('x')
    except Exception: pass
    try: child.close(force=True)
    except Exception: pass
sys.exit(rc)
