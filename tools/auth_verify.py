#!/usr/bin/env python3
"""authd end-to-end verification (decompose-native_shell step ②).

Boots the native-shell image in QEMU.  Checks that:
  1. authd loads the salted-hash credential from /ext4/etc/shadow,
  2. a WRONG password is denied (authd 'auth FAIL', shell 'Login incorrect'),
  3. the CORRECT password logs in (authd 'auth OK', shell 'Welcome, root!').

The shell itself holds no password — every check is an IPC call to authd.
Requires the disk seeded by tools/seed_auth.sh.

Exit 0 on success, non-zero otherwise.
"""
import os, sys

import pexpect

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.path.join(PROJ, "build/l4re_virt/images/bootstrap_native-shell.elf")
DISK = os.path.join(PROJ, "build/virt_disk.img")

net = ("-netdev user,id=net0 "
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
    # authd must load the on-disk credential (not a built-in fallback).
    child.expect(r"\[authd\] starting", timeout=90)
    idx = child.expect([r"\[authd\] loaded \d+ credential\(s\) from /ext4/etc/shadow",
                        r"\[authd\] using built-in fallback credentials"], timeout=30)
    if idx == 1:
        print("\n\n=== RESULT: FAIL — authd fell back, did not read /etc/shadow ===")
        raise SystemExit
    child.expect(r"\[authd\] service ready", timeout=10)

    # Login prompt.
    child.expect(r"login:", timeout=60)

    # 1) WRONG password must be denied.
    child.sendline("root")
    child.expect(r"[Pp]assword:", timeout=10)
    child.sendline("wrongpass")
    child.expect(r"\[authd\] auth FAIL for 'root' \(bad password\)", timeout=15)
    child.expect(r"Login incorrect", timeout=10)
    print("\n[check] wrong password correctly denied")

    # 2) CORRECT password must log in.
    child.expect(r"login:", timeout=10)
    child.sendline("root")
    child.expect(r"[Pp]assword:", timeout=10)
    child.sendline("12345678")
    child.expect(r"\[authd\] auth OK for 'root'", timeout=15)
    child.expect(r"Welcome, root!", timeout=10)
    child.expect(r"turingos>", timeout=10)
    print("\n\n=== RESULT: PASS — authd denied wrong pass, accepted correct (via IPC) ===")
    rc = 0
except pexpect.TIMEOUT:
    print("\n\n=== RESULT: FAIL — timeout waiting for expected output ===")
    rc = 3
except pexpect.EOF:
    print("\n\n=== RESULT: FAIL — QEMU exited early ===")
    rc = 4
except SystemExit:
    rc = 2
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
