#!/usr/bin/env bash
# seed_auth.sh — write /etc/shadow into the ext4 disk image for authd.
#
# authd reads /ext4/etc/shadow (format `user:salt_hex:hash_hex`, where
# hash = SHA-256(salt_bytes || password)).  This seeds one entry (root) so the
# guest authenticates against the on-disk hashed credential instead of anything
# compiled into the shell.  Uses debugfs (same approach as ext4_stream_test.sh).
#
# Usage: tools/seed_auth.sh [--disk PATH] [--user U] [--pass P] [--salt HEX]
set -euo pipefail
export PATH="/usr/sbin:/sbin:$PATH"   # debugfs / mkfs.ext4 live here

PROJ_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DISK="$PROJ_ROOT/build/virt_disk.img"
USER_NAME="root"
PASSWORD="12345678"
SALT_HEX="0123456789abcdef0123456789abcdef"

while [ $# -gt 0 ]; do
  case "$1" in
    --disk) DISK="$2"; shift 2;;
    --user) USER_NAME="$2"; shift 2;;
    --pass) PASSWORD="$2"; shift 2;;
    --salt) SALT_HEX="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

[ -f "$DISK" ] || { echo "disk image not found: $DISK" >&2; exit 1; }
command -v debugfs >/dev/null || { echo "debugfs not found (install e2fsprogs)" >&2; exit 1; }

HASH_HEX="$(python3 - "$SALT_HEX" "$PASSWORD" <<'EOF'
import hashlib, sys
salt = bytes.fromhex(sys.argv[1])
print(hashlib.sha256(salt + sys.argv[2].encode()).hexdigest())
EOF
)"

SHADOW_LINE="${USER_NAME}:${SALT_HEX}:${HASH_HEX}"
TMP="$(mktemp)"
printf '%s\n' "$SHADOW_LINE" > "$TMP"

echo "seeding /etc/shadow on $DISK:"
echo "  $SHADOW_LINE"

# Must be ONE debugfs session that cd's into /etc and writes a RELATIVE name:
# `write <src> /etc/shadow` with an absolute nested path leaks an inode without
# linking it (and leaves the fs inconsistent).  `mkdir /etc` is harmless if it
# already exists; `rm shadow` clears any prior entry so `write` doesn't dup.
debugfs -w "$DISK" >/dev/null 2>&1 <<EOF || true
mkdir /etc
cd /etc
rm shadow
write $TMP shadow
quit
EOF

rm -f "$TMP"

if debugfs -R 'cat /etc/shadow' "$DISK" 2>/dev/null | grep -q "^${USER_NAME}:"; then
  echo "done — /etc/shadow seeded."
else
  echo "ERROR: /etc/shadow not present after seeding" >&2
  exit 1
fi
