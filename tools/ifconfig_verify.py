#!/usr/bin/env python3
"""Verify the standalone ifconfig tool (net-cluster/tools) queries netd.

Boots, logs in, runs `run rom/ifconfig`, and checks the interface listing
(fetched from netd over IPC) shows vn0 with the DHCP/static IP and a MAC.
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
rc = 1
try:
    child.expect(r"\[netd\] stack ready", timeout=90)
    child.expect(r"login:", timeout=60)
    child.sendline("root")
    child.expect(r"[Pp]assword:", timeout=10)
    child.sendline("12345678")
    child.expect(r"turingos>", timeout=20)

    child.sendline("run rom/ifconfig")
    # netd formats the listing; expect the inet line + an ether line.
    child.expect(r"vn0:\s+flags=", timeout=30)
    child.expect(r"inet\s+10\.0\.2\.15\s+netmask", timeout=10)
    child.expect(r"ether\s+[0-9a-f:]{17}", timeout=10)
    print("\n\n=== RESULT: PASS — ifconfig tool printed netd's interface listing ===")
    rc = 0
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
