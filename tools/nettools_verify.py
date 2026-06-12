#!/usr/bin/env python3
"""Verify the migrated net tools (nslookup, ping, dhcp) run via netd.

Boots, logs in, and runs each `run rom/<tool>`, checking the netd-backed output.
"""
import os, re, sys
import pexpect

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.path.join(PROJ, "build/l4re_virt/images/bootstrap_native-shell.elf")
DISK = os.path.join(PROJ, "build/virt_disk.img")

disk = (f"-drive if=none,id=vdisk,file={DISK},format=raw "
        "-device virtio-blk-device,drive=vdisk,bus=virtio-mmio-bus.1")
cmd = (f"qemu-system-arm -M virt -cpu cortex-a15 -m 48M -smp 2 "
       f"-kernel {IMAGE} -display none -serial stdio -monitor none "
       f"-netdev user,id=net0 -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0 "
       f"{disk}")

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

    # nslookup: 10.0.2.3 is the slirp DNS; resolving the gateway literal works.
    child.sendline("run rom/nslookup 10.0.2.2")
    try:
        child.expect(r"Address:\s+10\.0\.2\.2", timeout=20)
        print("\n[check] nslookup OK")
    except pexpect.TIMEOUT:
        fails.append("nslookup")
    child.expect(r"turingos>", timeout=15)

    # ping the QEMU gateway.
    child.sendline("run rom/ping 10.0.2.2 -c 2")
    try:
        child.expect(r"bytes from 10\.0\.2\.2: icmp_seq=\d+", timeout=30)
        child.expect(r"packets transmitted", timeout=15)
        print("\n[check] ping OK")
    except pexpect.TIMEOUT:
        fails.append("ping")
    child.expect(r"turingos>", timeout=15)

    # dhcp renew.
    child.sendline("run rom/dhcp")
    try:
        child.expect(r"dhcp: IP 10\.0\.2\.\d+\s+netmask", timeout=30)
        print("\n[check] dhcp OK")
    except pexpect.TIMEOUT:
        fails.append("dhcp")
    child.expect(r"turingos>", timeout=15)

    if fails:
        print(f"\n\n=== RESULT: FAIL — {', '.join(fails)} ===")
        rc = 1
    else:
        print("\n\n=== RESULT: PASS — nslookup, ping, dhcp all work via netd ===")
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
