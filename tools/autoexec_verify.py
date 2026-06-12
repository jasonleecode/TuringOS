#!/usr/bin/env python3
"""Verify PATH-style auto-exec: a bare net-tool name runs rom/<cmd>; a typo
still reports 'command not found'."""
import os, sys
import pexpect

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.path.join(PROJ, "build/l4re_virt/images/bootstrap_native-shell.elf")
DISK = os.path.join(PROJ, "build/virt_disk.img")
disk = (f"-drive if=none,id=vdisk,file={DISK},format=raw "
        "-device virtio-blk-device,drive=vdisk,bus=virtio-mmio-bus.1")
cmd = (f"qemu-system-arm -M virt -cpu cortex-a15 -m 48M -smp 2 -kernel {IMAGE} "
       f"-display none -serial stdio -monitor none "
       f"-netdev user,id=net0 -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0 {disk}")

child = pexpect.spawn(cmd, encoding="utf-8", timeout=120); child.logfile = sys.stdout
rc = 0; fails = []
try:
    child.expect(r"\[netd\] stack ready", timeout=90)
    child.expect(r"login:", timeout=60); child.sendline("root")
    child.expect(r"[Pp]assword:", timeout=10); child.sendline("12345678")
    child.expect(r"turingos>", timeout=20)

    # bare 'ifconfig' must auto-exec rom/ifconfig (netd-backed)
    child.sendline("ifconfig")
    try:
        child.expect(r"vn0:\s+flags", timeout=25)
        child.expect(r"inet\s+10\.0\.2\.15", timeout=10)
        print("\n[check] bare 'ifconfig' auto-exec OK")
    except pexpect.TIMEOUT:
        fails.append("ifconfig auto-exec")
    child.expect(r"turingos>", timeout=10)

    # a typo still reports command not found
    child.sendline("nosuchcmd123")
    try:
        child.expect(r"nosuchcmd123: command not found", timeout=10)
        print("[check] typo -> command not found OK")
    except pexpect.TIMEOUT:
        fails.append("typo not-found")

    if fails:
        print(f"\n\n=== RESULT: FAIL — {fails} ==="); rc = 1
    else:
        print("\n\n=== RESULT: PASS — auto-exec by name + clean not-found ===")
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
