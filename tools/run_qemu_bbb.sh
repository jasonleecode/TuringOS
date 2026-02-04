#!/bin/bash
# QEMU 启动脚本 - BeagleBone Black L4Re

echo "=========================================="
echo "  TuringOS QEMU 测试"
echo "=========================================="

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"
ARTIFACTS="$PROJ_ROOT/build/artifacts"

if [ ! -f "$ARTIFACTS/bootstrap-image-bbb.elf" ]; then
    echo "错误: 找不到 bootstrap-image-bbb.elf"
    exit 1
fi

METHOD=${1:-1}

case $METHOD in
    1)
        echo "方法 1: ARM virt 机器 + cortex-a15"
        echo "按 Ctrl-A X 退出 QEMU"
        echo "------------------------------------------"
        qemu-system-arm \
            -M virt \
            -cpu cortex-a15 \
            -m 256M \
            -kernel "$ARTIFACTS/bootstrap-image-bbb.elf" \
            -nographic \
            -serial mon:stdio
        ;;
    2)
        echo "方法 2: Versatile Express A9"
        echo "按 Ctrl-A X 退出 QEMU"
        echo "------------------------------------------"
        qemu-system-arm \
            -M vexpress-a9 \
            -m 256M \
            -kernel "$ARTIFACTS/bootstrap-image-bbb.elf" \
            -nographic \
            -serial mon:stdio
        ;;
    3)
        echo "方法 3: 使用 device loader 精确加载到 0x80000000"
        echo "按 Ctrl-A X 退出 QEMU"
        echo "------------------------------------------"
        qemu-system-arm \
            -M virt \
            -cpu cortex-a15 \
            -m 512M \
            -device loader,file="$ARTIFACTS/bootstrap-image-bbb.elf",addr=0x80000000 \
            -nographic \
            -serial mon:stdio
        ;;
    *)
        echo "用法: $0 [1|2|3]"
        echo "  1 - ARM virt + cortex-a15 (默认)"
        echo "  2 - Versatile Express A9"
        echo "  3 - 使用 loader 精确加载"
        exit 1
        ;;
esac
