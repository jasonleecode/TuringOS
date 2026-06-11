#!/usr/bin/env python3
"""netd + netcat end-to-end verification (decompose-native_shell ① + ③).

Boots the native-shell image in QEMU, logs in, and runs the STANDALONE netcat
program as its own spawned process:  `run rom/netcat 10.0.2.2 5000 <msg>`.
netcat reaches the network purely over netd's Net_svr IPC (it links no lwIP and
holds no sigma0); the message must come back echoed (netcat -> IPC -> netd ->
lwIP -> NIC -> host -> back).  A host TCP echo server on 127.0.0.1:5000 is
reachable from the guest as 10.0.2.2:5000 via QEMU slirp.

Exit 0 on success, non-zero otherwise.
"""
import os, sys, socket, threading, re

import pexpect

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.path.join(PROJ, "build/l4re_virt/images/bootstrap_native-shell.elf")
DISK = os.path.join(PROJ, "build/virt_disk.img")
ECHO_PORT = 5000
MSG = "hello_netd_phase1"

# ---- host TCP echo server (guest dials 10.0.2.2:5000) ----
def echo_server(ready):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", ECHO_PORT))
    srv.listen(4)
    ready.set()
    srv.settimeout(90)
    try:
        while True:
            c, _ = srv.accept()
            data = c.recv(4096)
            if data:
                c.sendall(data)
            c.close()
    except Exception:
        pass
    finally:
        srv.close()

ready = threading.Event()
threading.Thread(target=echo_server, args=(ready,), daemon=True).start()
ready.wait(5)

net = ("-netdev user,id=net0,hostfwd=tcp::5555-:5000 "
       "-device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0")
disk = (f"-drive if=none,id=vdisk,file={DISK},format=raw "
        "-device virtio-blk-device,drive=vdisk,bus=virtio-mmio-bus.1")
cmd = (f"qemu-system-arm -M virt -cpu cortex-a15 -m 48M -smp 2 "
       f"-kernel {IMAGE} -display none -serial stdio -monitor none "
       f"{net} {disk}")

print("=== launching QEMU ===")
child = pexpect.spawn(cmd, encoding="utf-8", timeout=120)
child.logfile = sys.stdout

rc = 1
try:
    # netd should announce it grabbed the NIC via vbus + brought the stack up.
    child.expect(r"\[netd\] starting", timeout=90)
    child.expect(r"\[netd\] virtio-net at vbus MMIO", timeout=30)
    child.expect(r"\[netd\] stack ready", timeout=30)
    child.expect(r"\[netd\] service ready", timeout=10)

    # Login.
    child.expect(r"login:", timeout=60)
    child.sendline("root")
    child.expect(r"[Pp]assword:", timeout=10)
    child.sendline("12345678")
    child.expect(r"turingos>", timeout=20)

    # Run the standalone netcat program (its own process, network via netd IPC).
    child.sendline(f"run rom/netcat 10.0.2.2 {ECHO_PORT} {MSG}")
    idx = child.expect([r"netcat: recv \d+ bytes: " + re.escape(MSG),
                        r"netcat: netd unavailable",
                        r"netcat: connect.*failed",
                        r"netcat: send failed",
                        r"netcat: recv failed",
                        r"netcat: peer closed",
                        r"usage: netcat"], timeout=30)
    if idx == 0:
        print("\n\n=== RESULT: PASS — standalone netcat echoed via netd ===")
        rc = 0
    else:
        print(f"\n\n=== RESULT: FAIL — netcat error (branch {idx}) ===")
        rc = 2
except pexpect.TIMEOUT:
    print("\n\n=== RESULT: FAIL — timeout waiting for expected output ===")
    rc = 3
except pexpect.EOF:
    print("\n\n=== RESULT: FAIL — QEMU exited early ===")
    rc = 4
finally:
    try:
        child.sendcontrol('a'); child.send('x')
    except Exception:
        pass
    try:
        child.close(force=True)
    except Exception:
        pass

sys.exit(rc)
