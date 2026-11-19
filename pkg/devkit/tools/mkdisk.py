#!/usr/bin/env python3
"""Prepare a fresh QEMU virt development disk and print guest install commands."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
MAX_IMAGE = 4 * 1024 * 1024
IDENTIFIER = re.compile(r"[a-zA-Z0-9_-]{1,31}")


def stage(spec, source_tree):
    if len(spec) < 3:
        raise ValueError("--app requires NAME VERSION FILE [net data tmp]")
    name, version, filename, *grants = spec
    if not IDENTIFIER.fullmatch(name) or not IDENTIFIER.fullmatch(version):
        raise ValueError(f"invalid application name or version: {name} {version}")
    if set(grants) - {"net", "data", "tmp"} or len(grants) != len(set(grants)):
        raise ValueError(f"invalid or duplicate permission: {grants}")
    with Path(filename).open("rb") as f:
        image = f.read(MAX_IMAGE + 1)
    if len(image) > MAX_IMAGE:
        raise ValueError(f"{filename} exceeds the 4 MiB application limit")
    if image.startswith(b"\0asm\1\0\0\0"):
        if grants:
            raise ValueError("WASM modules use the runner's builtin imports")
        extension = "wasm"
    elif (len(image) >= 52 and image.startswith(b"\x7fELF\x01\x01\x01")
          and int.from_bytes(image[18:20], "little") == 40):
        extension = "elf"
    else:
        raise ValueError(f"{filename} must be an ARM32 little-endian ELF or a WASM v1 module")

    relative = Path("incoming") / name / f"{version}.{extension}"
    dest = source_tree / relative
    dest.parent.mkdir(parents=True, exist_ok=True)
    with dest.open("xb") as out:
        out.write(image)
    digest = hashlib.sha256(image).hexdigest()
    command = ["app", "install", name, version, f"/ext4/{relative}",
               *(grants or ["none"]), f"sha256={digest}"]
    return " ".join(command), len(image)


def make_disk(target, apps):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        raise FileExistsError(f"disk already exists: {target}")
    with tempfile.TemporaryDirectory(prefix=".sdk-disk-", dir=target.parent) as directory:
        work = Path(directory)
        tree = work / "root"
        commands, seen = [], set()
        total = 0
        for spec in apps:
            release = tuple(spec[:2])
            if release in seen:
                raise ValueError(f"duplicate release: {' '.join(release)}")
            seen.add(release)
            command, size = stage(spec, tree)
            commands.append(command)
            total += size
        (tree / "incoming/install.txt").write_text("\n".join(commands) + "\n")
        disk = work / "disk.img"
        # Keep room for both the incoming payloads and the installed copies.
        with disk.open("xb") as f:
            blocks = (3 * total + 4095) // 4096
            f.truncate(32 * 1024 * 1024 + 4096 * blocks)
        subprocess.run(["mkfs.ext4", "-q", "-F", "-b", "4096", "-O", "^has_journal",
                        "-d", tree, disk], check=True)
        subprocess.run([ROOT / "tools/seed_auth.sh", "--disk", disk],
                       check=True, stdout=subprocess.DEVNULL)
        os.link(disk, target)  # Publish only a finished image, without replacing an existing disk.
    return commands


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path, help="new raw ext4 disk image")
    parser.add_argument("--app", metavar="ARG", nargs="+", action="append", required=True,
                        help="NAME VERSION FILE [net data tmp]; repeat for multiple releases")
    args = parser.parse_args()
    try:
        commands = make_disk(args.disk, args.app)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"mkdisk: {exc}\n")
    print(f"Created {args.disk}. Guest install commands (/ext4/incoming/install.txt):")
    print("\n".join(commands))


if __name__ == "__main__":
    main()
