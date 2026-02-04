#!/bin/bash
# QEMU 启动脚本 - ARM Virt Platform

echo "=========================================="
echo "  TuringOS QEMU 测试 - Virt Platform"
echo "=========================================="

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"
ARTIFACTS="$PROJ_ROOT/build/artifacts"

if [ ! -f "$ARTIFACTS/bootstrap-image-virt.elf" ]; then
    echo "错误: 找不到 bootstrap-image-virt.elf"
    echo "请先运行: ./build.sh --board virt all"
    exit 1
fi

echo "启动 L4Re 系统..."
echo "按 Ctrl-A X 退出 QEMU"
echo "------------------------------------------"

qemu-system-arm \
    -M virt \
    -cpu cortex-a15 \
    -m 256M \
    -kernel "$ARTIFACTS/bootstrap-image-virt.elf" \
    -nographic \
    -serial mon:stdio
