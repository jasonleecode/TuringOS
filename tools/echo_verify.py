#!/usr/bin/env python3
"""Verify the migrated tcpecho/udpecho tools (netd Phase 2 echo servers).

Boots, logs in, starts each echo server via its tool, then connects from the
host (through QEMU hostfwd) and checks the data is echoed back:
  run rom/tcpecho -> netd TCP echo on :5000 (host :5555) -> echo
  run rom/udpecho -> netd UDP echo on :5001 (host :5556) -> echo
"""
import os, sys, socket, time
import pexpect

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.path.join(PROJ, "build/l4re_virt/images/bootstrap_native-shell.elf")
DISK = os.path.join(PROJ, "build/virt_disk.img")

net = ("-netdev user,id=net0,hostfwd=tcp::5555-:5000,hostfwd=udp::5556-:5001 "
       "-device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0")
disk = (f"-drive if=none,id=vdisk,file={DISK},format=raw "
        "-device virtio-blk-device,drive=vdisk,bus=virtio-mmio-bus.1")
cmd = (f"qemu-system-arm -M virt -cpu cortex-a15 -m 48M -smp 2 "
       f"-kernel {IMAGE} -display none -serial stdio -monitor none {net} {disk}")

def tcp_probe(msg, tries=10):
    for _ in range(tries):
        try:
            c = socket.create_connection(("127.0.0.1", 5555), timeout=3)
            c.recv(256)                     # banner
            c.sendall(msg)
            data = c.recv(256); c.close()
            return data
        except Exception:
            time.sleep(0.5)
    return b""

def udp_probe(msg, tries=10):
    for _ in range(tries):
        try:
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            u.settimeout(2)
            u.sendto(msg, ("127.0.0.1", 5556))
            data, _ = u.recvfrom(256); u.close()
            return data
        except Exception:
            time.sleep(0.5)
    return b""

child = pexpect.spawn(cmd, encoding="utf-8", timeout=120)
child.logfile = sys.stdout
rc = 0
fails = []
try:
    child.expect(r"\[netd\] stack ready", timeout=90)
    child.expect(r"login:", timeout=60)
    child.sendline("root")
    child.expect(r"[Pp]assword:", timeout=10)
    child.sendline("12345678")
    child.expect(r"turingos>", timeout=20)

    child.sendline("run rom/tcpecho")
    child.expect(r"TCP echo server (started|already)", timeout=20)
    child.expect(r"turingos>", timeout=10)
    echoed = tcp_probe(b"hello_tcp")
    print(f"\n[host] tcp echo got: {echoed!r}")
    if b"hello_tcp" not in echoed: fails.append("tcpecho")
    else: print("[check] tcpecho OK")

    child.sendline("run rom/udpecho")
    child.expect(r"echo server (started|already)", timeout=20)
    child.expect(r"turingos>", timeout=10)
    echoed = udp_probe(b"hello_udp")
    print(f"\n[host] udp echo got: {echoed!r}")
    if b"hello_udp" not in echoed: fails.append("udpecho")
    else: print("[check] udpecho OK")

    if fails:
        print(f"\n\n=== RESULT: FAIL — {', '.join(fails)} ===")
        rc = 1
    else:
        print("\n\n=== RESULT: PASS — tcpecho + udpecho echo via netd worker threads ===")
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
