#!/bin/bash
# QEMU 启动脚本 - ARM Virt Platform

echo "=========================================="
echo "  TuringOS QEMU 测试 - Virt Platform"
echo "=========================================="

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"

# 优先使用 ARM Virt 镜像，如果不存在则使用 ARM64 RPi4 镜像
BUILDS=(
    "$PROJ_ROOT/build/l4re_virt/images/bootstrap.elf"
    "$PROJ_ROOT/build/l4re_arm64/images/bootstrap.elf"
    "$PROJ_ROOT/build/artifacts/bootstrap-image-virt.elf"
    "$PROJ_ROOT/build/artifacts/bootstrap-final-rpi4.elf"
)

IMAGE=""
for img in "${BUILDS[@]}"; do
    if [ -f "$img" ]; then
        IMAGE="$img"
        echo "找到镜像: $IMAGE"
        break
    fi
done

if [ -z "$IMAGE" ]; then
    echo "错误: 找不到引导镜像"
    echo ""
    echo "请先运行构建:"
    echo "  ./build.sh              # 构建 ARM64 (RPi4) 镜像"
    echo ""
    echo "或者直接使用 QEMU 启动已构建的镜像:"
    echo "  qemu-system-aarch64 -M virt -cpu cortex-a57 -m 1024M -nographic \\"
    echo "    -kernel /Users/jason/Documents/opensource/turingos/build/l4re_arm64/images/bootstrap.elf"
    exit 1
fi

# 检测镜像架构
MACHINE_TYPE=""
if echo "$IMAGE" | grep -q "l4re_arm64\|rpi4"; then
    MACHINE_TYPE="aarch64"
    QEMU_ARCH="qemu-system-aarch64"
    CPU="-cpu cortex-a57"
    MEM="-m 1024M"
    MACHINE="-M virt,virtualization=true"
elif echo "$IMAGE" | grep -q "l4re_virt"; then
    MACHINE_TYPE="arm"
    QEMU_ARCH="qemu-system-arm"
    CPU="-cpu cortex-a15"
    MEM="-m 256M"
    MACHINE="-M virt"
else
    # 默认使用 ARM64
    MACHINE_TYPE="aarch64"
    QEMU_ARCH="qemu-system-aarch64"
    CPU="-cpu cortex-a57"
    MEM="-m 1024M"
    MACHINE="-M virt,virtualization=true"
fi

echo ""
echo "使用架构: $MACHINE_TYPE"
echo "镜像路径: $IMAGE"
echo ""
echo "启动 L4Re 系统..."
echo "按 Ctrl-A X 退出 QEMU"
echo "------------------------------------------"

$QEMU_ARCH \
    $MACHINE \
    $CPU \
    $MEM \
    -kernel "$IMAGE" \
    -nographic \
    -serial mon:stdio
