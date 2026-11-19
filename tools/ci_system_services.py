#!/usr/bin/env python3
"""Exercise the virt system image using a fresh disposable disk across two boots."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

import pexpect

ROOT = Path(__file__).resolve().parents[1]
IMAGE = Path(os.environ.get("BUILD_OUT", ROOT / "build")) / "artifacts/virt/native-shell/bootstrap.elf"


def prepare(work):
    disk = work / "disk.img"
    with disk.open("wb") as f:
        f.truncate(32 << 20)
    subprocess.run(["mkfs.ext4", "-q", "-F", "-b", "4096", "-O", "^has_journal", disk], check=True)
    subprocess.run([ROOT / "tools/seed_auth.sh", "--disk", disk], check=True, stdout=subprocess.DEVNULL)
    # Exported main() returns the supplied small integer.
    module = bytes.fromhex("0061736d01000000 0105016000017f 03020100 070801046d61696e0000 0a0601040041")
    fixtures = {"seven.wasm": module + bytes([7, 11]), "nine.wasm": module + bytes([9, 11]),
                "no-main.wasm": module[:8], "large.txt": b"1234567890abcdef" * 1024 + b"\n"}
    for name, payload in fixtures.items():
        (work / name).write_bytes(payload)
        subprocess.run(["debugfs", "-w", "-R", f"write {name} {name}", disk],
                       cwd=work, check=True, capture_output=True)
    return disk


def boot(work, disk, second=False):
    child = pexpect.spawn("qemu-system-arm", [
        "-M", "virt", "-cpu", "cortex-a15", "-smp", "2", "-m", "48M",
        "-display", "none", "-monitor", "none", "-serial", "stdio", "-kernel", str(IMAGE),
        "-drive", f"if=none,id=d,file={disk},format=raw",
        "-device", "virtio-blk-device,drive=d,bus=virtio-mmio-bus.1",
        "-netdev", "user,id=n,guestfwd=tcp:10.0.2.100:9000-cmd:/bin/cat",
        "-device", "virtio-net-device,netdev=n,bus=virtio-mmio-bus.0",
    ], encoding="utf-8", codec_errors="replace", timeout=30)

    def command(line, *patterns):
        child.sendline(line)
        for pattern in patterns:
            child.expect(pattern)
        child.expect_exact("turingos>")
        print("PASS", line, flush=True)

    with (work / "serial.log").open("a") as log:
        child.logfile_read = log
        try:
            child.expect_exact("login:")
            child.sendline("root")
            child.expect_exact("Password:")
            child.sendline("12345678")
            child.expect_exact("turingos>")
            if second:
                command("run app run calc", r"run: task exited with code: 7")
                command("app install calc v2 /ext4/nine.wasm", "installed calc v2")
                digest = hashlib.sha256((work / "nine.wasm").read_bytes()).hexdigest()
                command(f"echo damaged >/ext4/apps/calc.{digest}", r"op_release: freed handle")
                command("run app run calc", r"run: task exited with code: 7")
                command("cat /tmp/first", r"No such file")
                return
            command("temp", r"[0-9]+\.[0-9]+ °C")
            child.sendline("svc restart temp0")
            child.expect(r"temp0 stopped")
            child.expect(r"registered as dev/temp0")
            command("temp", r"[0-9]+\.[0-9]+ °C")
            command("help", r"ROM programs:", r"wasm-run")
            command("tmpfstest", r"tmpfstest: PASS")
            command("echo 'pipe one'|cat|cat >/tmp/first", r"op_release: freed handle for '/first'")
            command("echo '|' | cat", r"\| \|")
            command("echo two >>/tmp/first", r"op_release: freed handle for '/first'")
            command("cat </tmp/first|cat", r"\| pipe one", r"\| two")
            command("cat /ext4/large.txt | cat >/ext4/copy.txt", r"op_release: freed handle for '/copy.txt'")
            command("ifconfig | cat", r"\| vn0: flags=")
            command("netcat 10.0.2.100 9000 probe", r"netcat: recv 5 bytes: probe")
            command("exectest --caps", "netd=0 data=0 ext4=0 tmp=0 authd=0")
            command("app install logger v1 /rom/exectest data", "installed logger v1")
            command("run app run logger --caps /data/log", "netd=0 data=1 ext4=0 tmp=0 authd=0",
                    r"run: task exited with code: 0")
            command("run app run logger --caps /ext4/private.txt", r"run: task exited with code: 1")
            command("cat /ext4/data/log", r"\| application data")
            command("run wasm-run /ext4/seven.wasm", r"run: task exited with code: 7")
            command("app install calc v1 /ext4/seven.wasm", "installed calc v1")
            command("app install calc v2 /ext4/nine.wasm", "installed calc v2")
            command("run app run calc", r"run: task exited with code: 9")
            command("run app install calc bad /ext4/seven.wasm sha256=abc", "checksum mismatch",
                    r"run: task exited with code: 1")
            command("run app install calc broken /ext4/no-main.wasm", "invalid entry", r"run: task exited with code: 1")
            command("app install args v1 /rom/echo", "installed args v1")
            command("app run args nested-output | cat", r"\| nested-output")
            command("app rollback calc", "rolled back calc to v1")
            command("app status calc", r"calc v1 wasm permissions=none")
        except (pexpect.EOF, pexpect.TIMEOUT):
            log.flush()
            raise RuntimeError(f"guest did not produce expected output\n{child.before[-2500:]}") from None
        finally:
            child.close(force=True)


def main():
    with tempfile.TemporaryDirectory(prefix="turingos-services-") as directory:
        work = Path(directory)
        disk = prepare(work)
        boot(work, disk)
        boot(work, disk, second=True)
        result = subprocess.run(["debugfs", "-R", "cat /copy.txt", disk], check=True, capture_output=True)
        assert hashlib.sha256(result.stdout).digest() == hashlib.sha256((work / "large.txt").read_bytes()).digest()
        assert not re.search(r"unhandled .*page fault|FATAL: Fatal signal", (work / "serial.log").read_text())
        print("PASS reboot, persistent rollback, volatile tmp, exact 16 KiB pipe transfer")


if __name__ == "__main__":
    main()
