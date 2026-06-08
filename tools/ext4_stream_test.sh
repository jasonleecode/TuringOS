#!/bin/bash
#
# ext4_stream_test.sh — end-to-end test for the ext4 streaming-I/O redesign
# ============================================================================
#
# Boots the native-shell image headless, drives the shell over serial, and
# verifies (by extracting files from the disk image with debugfs — robust,
# not serial-parsing):
#
#   1. write-through + create + read : echo > /ext4/t.txt round-trips
#   2. O_TRUNC overwrite             : `echo bb >` over a longer file leaves
#                                      exactly "bb\n" — no stale tail
#   3. >bufsize / >4 MiB streaming   : `cat big > copy` of an N-MiB file is
#                                      byte-exact (cmp) — proves the old 4 MiB
#                                      cap is gone and offset read+write past
#                                      the boundary are correct
#   4. no kernel crash / JDB during the run
#
# All checks must pass for exit 0 (suitable as a regression gate).
#
# The N-MiB source is generated on the host and injected into the disk image
# with debugfs; the guest copies it; the copy is extracted and compared.
#
# Usage: tools/ext4_stream_test.sh [-s SIZE_MB] [-t TIMEOUT] [--keep]
#   -s N        big-file size in MiB        (default 6; must exceed 4 to test
#               the old cap, and exceed the 64 KiB server buffer to test chunking)
#   -t SEC      overall QEMU wall timeout   (default scales with size)
#   --image P   boot image                  (default bootstrap_native-shell.elf)
#   --disk P    ext4 disk image             (default build/virt_disk.img;
#               created + mkfs.ext4'd if absent)
#   --keep      keep the work dir + serial log on success (for debugging)
#
# ============================================================================
set -u

export PATH="/usr/sbin:/sbin:$PATH"   # debugfs / mkfs.ext4 live here

PROJ_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="$PROJ_ROOT/build/l4re_virt/images/bootstrap_native-shell.elf"
DISK="$PROJ_ROOT/build/virt_disk.img"
SIZE_MB=6
TIMEOUT=0          # 0 => auto from size
KEEP=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -s)        SIZE_MB="$2"; shift 2 ;;
        -t)        TIMEOUT="$2"; shift 2 ;;
        --image)   IMAGE="$2"; shift 2 ;;
        --disk)    DISK="$2"; shift 2 ;;
        --keep)    KEEP=1; shift ;;
        -h|--help) sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//; /^set -u/d'; exit 0 ;;
        *) echo "未知参数: $1 (--help 查看用法)"; exit 2 ;;
    esac
done

# Copy of an N-MiB file needs time proportional to size; size the waits/timeout.
COPY_WAIT=$(( SIZE_MB * 4 + 8 ))
[ "$TIMEOUT" -eq 0 ] && TIMEOUT=$(( COPY_WAIT + 30 ))

# ---- preconditions --------------------------------------------------------
for t in qemu-system-arm debugfs; do
    command -v "$t" >/dev/null 2>&1 || { echo "错误: 找不到 $t"; exit 2; }
done
if [ ! -f "$IMAGE" ]; then
    echo "错误: 找不到镜像 $IMAGE"
    echo "请先构建: ./build.sh --board virt all"
    exit 2
fi
if [ "$SIZE_MB" -le 4 ]; then
    echo "警告: -s $SIZE_MB <= 4，无法证明 4 MiB 上限已破（建议 >= 6）。"
fi

# Create + format the disk if it does not exist (mirrors run_qemu_virt.sh).
if [ ! -f "$DISK" ]; then
    command -v mkfs.ext4 >/dev/null 2>&1 || { echo "错误: 磁盘不存在且找不到 mkfs.ext4"; exit 2; }
    echo ">>> 创建并格式化磁盘 $DISK (100M)"
    mkdir -p "$(dirname "$DISK")"
    qemu-img create -f raw "$DISK" 100M >/dev/null
    mkfs.ext4 -q -b 4096 -O ^has_journal -F "$DISK"
fi

WORK="$(mktemp -d /tmp/ext4_stream_test.XXXXXX)"
SRC="$WORK/big.bin"
LOG="$WORK/serial.log"

cleanup_disk() {
    # Remove this test's files from the (persistent) disk image.
    for f in big.bin big_copy.bin t.txt f.txt; do
        debugfs -w -R "rm /$f" "$DISK" >/dev/null 2>&1
    done
}

fail() { echo "  ✗ $1"; FAILED=1; }
ok()   { echo "  ✓ $1"; }

# ---- prepare --------------------------------------------------------------
echo "=========================================="
echo " ext4 流式 I/O 端到端测试"
echo "=========================================="
echo " 镜像   : $IMAGE"
echo " 磁盘   : $DISK"
echo " 大文件 : ${SIZE_MB} MiB    超时: ${TIMEOUT}s"
echo "------------------------------------------"

# Generate the N-MiB source with per-line counters (corruption-detectable).
python3 - "$SRC" "$SIZE_MB" <<'PY'
import sys
path, mb = sys.argv[1], int(sys.argv[2])
n = mb * 1024 * 1024
data = bytearray()
i = 0
while len(data) < n:
    data += b"%08d-ext4-streaming-test-line\n" % i
    i += 1
del data[n:]
open(path, "wb").write(data)
PY

cleanup_disk                                   # clear stale outputs first
debugfs -w -R "write $SRC big.bin" "$DISK" >/dev/null 2>&1 \
    || { echo "错误: 注入 big.bin 失败"; rm -rf "$WORK"; exit 2; }

# ---- drive the guest ------------------------------------------------------
echo ">>> 启动 QEMU 并驱动 shell ..."
(
  sleep 9;  printf 'root\n'
  sleep 1;  printf '12345678\n'
  sleep 3;  printf 'echo hi_streaming > /ext4/t.txt\n'        # 1) write-through
  sleep 2;  printf 'echo aaaaaaaaaaaaaaaa > /ext4/f.txt\n'    # 2) longer file...
  sleep 2;  printf 'echo bb > /ext4/f.txt\n'                  #    ...O_TRUNC overwrite
  sleep 2;  printf 'cat /ext4/big.bin > /ext4/big_copy.bin\n' # 3) N-MiB streaming copy
  sleep "$COPY_WAIT"
  printf 'echo STREAM_TEST_DONE\n'
  sleep 3
) | timeout "$TIMEOUT" qemu-system-arm -M virt -cpu cortex-a15 -m 256M -smp 2 \
      -kernel "$IMAGE" -display none -serial stdio -no-reboot \
      -netdev user,id=net0 -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0 \
      -drive if=none,id=vdisk,file="$DISK",format=raw \
      -device virtio-blk-device,drive=vdisk,bus=virtio-mmio-bus.1 \
      > "$LOG" 2>&1

# ---- verify (extract from disk, compare) ----------------------------------
echo "------------------------------------------"
FAILED=0

# 4) crash check
if grep -qaiE "schedule_in_progress|Assertion|Return reboots|enters the L4 kernel debugger" "$LOG"; then
    fail "运行中出现内核崩溃/JDB"
else
    ok "无内核崩溃"
fi

# 1) write-through round-trip
debugfs -R "dump /t.txt $WORK/t.txt" "$DISK" >/dev/null 2>&1
if [ -f "$WORK/t.txt" ] && [ "$(cat "$WORK/t.txt")" = "hi_streaming" ]; then
    ok "写穿透 + 读回: t.txt = 'hi_streaming'"
else
    fail "写穿透: t.txt = '$(cat "$WORK/t.txt" 2>/dev/null)' (期望 'hi_streaming')"
fi

# 2) O_TRUNC overwrite leaves exactly "bb\n" (3 bytes, no stale tail)
debugfs -R "dump /f.txt $WORK/f.txt" "$DISK" >/dev/null 2>&1
fsz=$(stat -c%s "$WORK/f.txt" 2>/dev/null || echo -1)
if [ "$(cat "$WORK/f.txt" 2>/dev/null)" = "bb" ] && [ "$fsz" = "3" ]; then
    ok "O_TRUNC 覆盖: f.txt = 'bb' (3 字节，无残留)"
else
    fail "O_TRUNC: f.txt size=$fsz 内容='$(cat "$WORK/f.txt" 2>/dev/null)' (期望 'bb' / 3 字节)"
fi

# 3) >4 MiB streaming copy is byte-exact
debugfs -R "dump /big_copy.bin $WORK/big_copy.bin" "$DISK" >/dev/null 2>&1
if [ ! -f "$WORK/big_copy.bin" ]; then
    fail "大文件拷贝: big_copy.bin 不存在（拷贝未完成，可加大 -t）"
elif cmp -s "$SRC" "$WORK/big_copy.bin"; then
    ok "大文件流式: ${SIZE_MB} MiB 拷贝字节一致（4 MiB 上限已破）"
else
    csz=$(stat -c%s "$WORK/big_copy.bin")
    fail "大文件流式: cmp 不一致 (copy=$csz src=$(stat -c%s "$SRC"))"
fi

# ---- report ---------------------------------------------------------------
cleanup_disk
echo "------------------------------------------"
if [ "$FAILED" -eq 0 ]; then
    [ "$KEEP" -eq 0 ] && rm -rf "$WORK"
    echo "PASS — ext4 流式 I/O 全部验证通过"
    exit 0
else
    echo "FAIL — 详见串口日志: $LOG"
    echo "（工作目录保留: $WORK）"
    exit 1
fi
