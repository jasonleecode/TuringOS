#!/usr/bin/env python3
"""Verify the cleaned-up root listing (dev dedup + /svc + /dev nodes).

Boots, logs in, and checks:
  ls /     -> dev ext4 proc rom svc sys tmp ; exactly one 'dev'; NO loose
              service/device gate caps (authd spawnd syslogd fb input rtc)
  ls /svc  -> authd spawnd syslogd
  ls /dev  -> rtc0 input0 null zero
"""
import os, re, sys
import pexpect

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.path.join(PROJ, "build/l4re_virt/images/bootstrap_native-shell.elf")
DISK = os.path.join(PROJ, "build/virt_disk.img")

# GPU mode (LS_VERIFY_GPU=1): add the ramfb framebuffer + fw_cfg resolution so
# bootstrap registers the "vesa" cap, fb-drv starts, and /dev/fb0 appears.
GPU = os.environ.get("LS_VERIFY_GPU") == "1"
gpu_args = ("-device ramfb -fw_cfg name=opt/org.l4re/fb_res,string=800x600"
            if GPU else "")

disk = (f"-drive if=none,id=vdisk,file={DISK},format=raw "
        "-device virtio-blk-device,drive=vdisk,bus=virtio-mmio-bus.1")
cmd = (f"qemu-system-arm -M virt -cpu cortex-a15 -m 48M -smp 2 "
       f"-kernel {IMAGE} -display none -serial stdio -monitor none "
       f"-netdev user,id=net0 -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0 "
       f"{gpu_args} {disk}")

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
def clean(s):
    s = ANSI.sub("", s)
    s = s.replace("\r", "\n").replace("\b", "")
    return s

def run_ls(child, path, marker):
    # The readline redraw echoes "turingos>" on every keystroke, so we can't
    # expect the prompt to delimit output.  Instead bracket with an echo marker
    # whose OUTPUT appears once after the ls output; child.before up to the
    # marker's first (command-echo) occurrence already contains the ls output.
    cmdline = f"ls {path}".strip()
    child.sendline(cmdline)
    child.sendline(f"echo {marker}")
    child.expect(marker, timeout=15)
    out = clean(child.before)
    child.expect(r"turingos>", timeout=15)
    # The redraw leaves no newline before the output, so the listing is glued
    # onto the final "...ls <path>" echo.  Slice after the last command echo and
    # stop at the next prompt.
    idx = out.rfind(cmdline)
    seg = out[idx + len(cmdline):] if idx >= 0 else out
    cut = seg.find("turingos")
    if cut >= 0:
        seg = seg[:cut]
    toks = set(t.rstrip("/") for t in seg.split() if t.strip())
    return toks, seg

child = pexpect.spawn(cmd, encoding="utf-8", timeout=120)
child.logfile = sys.stdout
rc = 0
fails = []
try:
    child.expect(r"login:", timeout=90)
    child.sendline("root")
    child.expect(r"[Pp]assword:", timeout=10)
    child.sendline("12345678")
    child.expect(r"turingos>", timeout=20)

    root, root_raw = run_ls(child, "/",    "Z9ROOT")
    svc,  _        = run_ls(child, "/svc", "Z9SVC")
    dev,  _        = run_ls(child, "/dev", "Z9DEV")

    print("\n\n--- parsed root tokens:", sorted(root))
    print("--- parsed /svc tokens:", sorted(svc))
    print("--- parsed /dev tokens:", sorted(dev))

    # root must have the filesystem dirs, must NOT have loose service/device caps
    for want in ("dev", "ext4", "proc", "rom", "svc", "sys", "tmp"):
        if want not in root: fails.append(f"root missing '{want}'")
    for bad in ("authd", "spawnd", "syslogd", "fb", "input", "rtc"):
        if bad in root: fails.append(f"root still shows loose cap '{bad}'")
    # exactly one 'dev' in the root listing (dedup)
    ndev = len(re.findall(r"\bdev\b", root_raw))
    if ndev != 1: fails.append(f"root has {ndev} 'dev' entries (want 1)")

    for want in ("authd", "spawnd", "syslogd"):
        if want not in svc: fails.append(f"/svc missing '{want}'")
    dev_want = ("rtc0", "input0", "null", "zero")
    if GPU:
        dev_want += ("fb0",)   # framebuffer present in --gpu mode
    for want in dev_want:
        if want not in dev: fails.append(f"/dev missing '{want}'")
    if not GPU and "fb0" in dev:
        fails.append("/dev shows 'fb0' without a framebuffer")

    if fails:
        print("\n\n=== RESULT: FAIL ===")
        for f in fails: print("  -", f)
        rc = 1
    else:
        print("\n\n=== RESULT: PASS — root deduped & decluttered; /svc and /dev populated ===")
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
