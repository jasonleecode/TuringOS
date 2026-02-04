#!/bin/bash
# QEMU 调试启动脚本

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"
ARTIFACTS="$PROJ_ROOT/build/artifacts"

echo "=========================================="
echo "  QEMU 调试模式"
echo "=========================================="

# 方法：使用 -d 参数输出调试信息
timeout 5 qemu-system-arm \
    -M virt \
    -cpu cortex-a15 \
    -m 256M \
    -kernel "$ARTIFACTS/bootstrap-image-bbb.elf" \
    -nographic \
    -serial mon:stdio \
    -d guest_errors,unimp,cpu_reset \
    -D /tmp/qemu_debug.log 2>&1

echo ""
echo "--- QEMU 调试日志 ---"
if [ -f /tmp/qemu_debug.log ]; then
    cat /tmp/qemu_debug.log
else
    echo "无调试日志"
fi
