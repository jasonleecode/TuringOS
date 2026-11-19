#!/usr/bin/env python3
"""Verify telnetd: a remote native_shell over telnet (netd + spawnd vcon bridge).

Boots, logs in on the serial console, starts `run rom/telnetd`, then connects a
host telnet client (through QEMU hostfwd tcp:127.0.0.1:2323-:23), logs in over telnet, and
runs a command — proving the vcon bridge + spawn_con give a real remote shell.
"""
import os, sys, socket, tempfile
import pexpect

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.path.join(PROJ, "build/l4re_virt/images/bootstrap_native-shell.elf")
DISK = os.environ.get("TURINGOS_TEST_DISK", os.path.join(PROJ, "build/virt_disk.img"))

net = ("-netdev user,id=net0,hostfwd=tcp:127.0.0.1:2323-:23 "
       "-device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0")
disk = (f"-drive if=none,id=vdisk,file={DISK},format=raw,snapshot=on "
        "-device virtio-blk-device,drive=vdisk,bus=virtio-mmio-bus.1")
cmd = (f"qemu-system-arm -M virt -cpu cortex-a15 -m 48M -smp 2 "
       f"-kernel {IMAGE} -display none -serial stdio -monitor none {net} {disk}")

def strip_iac(data: bytes) -> bytes:
    out = bytearray(); i = 0
    while i < len(data):
        b = data[i]
        if b == 255:                      # IAC
            if i + 1 < len(data) and 251 <= data[i+1] <= 254:
                i += 3                    # IAC WILL/WONT/DO/DONT opt
            else:
                i += 2
            continue
        out.append(b); i += 1
    return bytes(out)

class Tn:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.settimeout(10); self.buf = b""
        self.last_match = b""
    def pump(self, t=0.4):
        self.s.settimeout(t)
        try:
            while True:
                d = self.s.recv(4096)
                if not d: break
                self.buf += strip_iac(d)
        except socket.timeout:
            pass
        self.s.settimeout(10)
        return self.buf
    def expect(self, text, tries=20):
        for _ in range(tries):
            self.pump(0.5)
            token = text.encode()
            index = self.buf.find(token)
            if index >= 0:
                self.last_match = self.buf[:index + len(token)]
                self.buf = self.buf[index + len(token):]
                return True
        return False
    def send(self, s):
        self.s.sendall(s.encode())

child = pexpect.spawn(cmd, encoding="utf-8", timeout=120,
                      env={**os.environ, "TMPDIR": tempfile.gettempdir()})
child.logfile = sys.stdout
rc = 1
try:
    # --- serial: log in and start telnetd ---
    child.expect(r"\[netd\] stack ready", timeout=90)
    child.expect(r"login:", timeout=60)
    child.sendline("root"); child.expect(r"[Pp]assword:", timeout=10)
    child.sendline("12345678"); child.expect(r"turingos>", timeout=20)
    pager_error = r"Pager of [0-9a-fA-F]+ is invalid"

    def expect_clean(pattern):
        if child.expect([pager_error, pattern], timeout=20) == 0:
            raise SystemExit("kernel reported an invalid pager during the session")

    def start_session():
        child.sendline("run rom/telnetd &")
        expect_clean(r"spawned: rom/telnetd \(handle=(\d+) task=")
        daemon_handle = child.match.group(1)
        expect_clean(r"telnetd: listening on port 23")
        tn = Tn("127.0.0.1", 2323)
        expect_clean(r"telnetd: shell session started \(handle (\d+)\)")
        return tn, daemon_handle, child.match.group(1)

    def disconnect(tn, daemon_handle, shell_handle):
        tn.s.close()
        expect_clean(r"reaper: \[" + shell_handle + r"\] 'rom/native_shell' exited \(code=0\)")
        expect_clean(r"reaper: \[" + daemon_handle + r"\] 'rom/telnetd' exited \(code=0\)")
        child.sendline(f"wait {daemon_handle}")
        expect_clean(r"\[" + daemon_handle + r"\] done  exit code: 0")

    tn, daemon_handle, shell_handle = start_session()
    if not tn.expect("login:"):
        print("\n[telnet] buffer so far:", tn.buf[-400:]); raise SystemExit("no login over telnet")
    print("[telnet] got login prompt over telnet")
    tn.send("root\r\n")
    if not tn.expect("Password:"): raise SystemExit("no password prompt over telnet")
    tn.send("12345678\r\n")
    if not tn.expect("Welcome, root!"): raise SystemExit("login over telnet failed")
    if b"12345678" in tn.last_match: raise SystemExit("password echoed by remote console")
    print("[telnet] logged in over telnet")
    if not tn.expect("turingos>"): raise SystemExit("no shell prompt")
    tn.send("uname\r\n")
    if not tn.expect("TuringOS  Fiasco/L4Re  armv7"):
        print("\n[telnet] buffer:", tn.buf[-400:]); raise SystemExit("command output not seen over telnet")
    tn.send(f"wait {daemon_handle}\r\n")
    if not tn.expect(f"wait: cannot wait for handle {daemon_handle} (err=-13)"):
        raise SystemExit("session could wait on a task it does not own")
    tn.send("ifconfig\r\n")
    if not tn.expect("10.0.2.15"):
        raise SystemExit("external command output missing from remote session")
    disconnect(tn, daemon_handle, shell_handle)

    # Disconnect at authentication as well, on the same guest and port.
    tn, daemon_handle, shell_handle = start_session()
    if not tn.expect("login:"): raise SystemExit("second login prompt missing")
    tn.send("root\r\n")
    if not tn.expect("Password:"): raise SystemExit("second password prompt missing")
    disconnect(tn, daemon_handle, shell_handle)
    print("\n\n=== RESULT: PASS — remote login, uname and session cleanup ===")
    rc = 0
except SystemExit as e:
    print(f"\n\n=== RESULT: FAIL — {e} ==="); rc = 2
except pexpect.TIMEOUT:
    print("\n\n=== RESULT: FAIL — serial timeout ==="); rc = 3
except pexpect.EOF:
    print("\n\n=== RESULT: FAIL — QEMU exited early ==="); rc = 4
finally:
    try: child.sendcontrol('a'); child.send('x')
    except Exception: pass
    try: child.close(force=True)
    except Exception: pass
sys.exit(rc)
