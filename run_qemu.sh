#!/bin/bash
# TuringOS QEMU 启动测试脚本
# 用法:
#   ./run_qemu.sh              # 启动默认镜像 (hello)
#   ./run_qemu.sh <entry>      # 启动指定 entry 的镜像
#   ./run_qemu.sh --timeout 5  # 设置超时秒数 (0=不限)
#   ./run_qemu.sh --check      # 仅验证镜像, 不启动 QEMU

set -e

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJ_ROOT/l4re/build_arm64"
IMAGES_DIR="$BUILD_DIR/images"
KCONFIG="$BUILD_DIR/.kconfig"

ENTRY="hello"
TIMEOUT=10
CHECK_ONLY=0

# 解析参数
while [[ $# -gt 0 ]]; do
    case "$1" in
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --check)   CHECK_ONLY=1; shift ;;
        *)         ENTRY="$1"; shift ;;
    esac
done

IMAGE="$IMAGES_DIR/bootstrap_${ENTRY}.elf"
if [ ! -f "$IMAGE" ]; then
    IMAGE="$IMAGES_DIR/bootstrap.elf"
fi

if [ ! -f "$IMAGE" ]; then
    echo "Error: 找不到引导镜像"
    echo "  尝试过: $IMAGES_DIR/bootstrap_${ENTRY}.elf"
    echo "  尝试过: $IMAGES_DIR/bootstrap.elf"
    echo "请先运行: ./build.sh l4re"
    echo "  然后:   cd $BUILD_DIR && CROSS_COMPILE=aarch64-elf- make E=$ENTRY elfimage"
    exit 1
fi

# ============================================================
# 镜像验证
# ============================================================
echo "========================================"
echo "  TuringOS QEMU 启动测试"
echo "  镜像: $(basename "$IMAGE") ($(du -h "$IMAGE" | cut -f1))"
echo "========================================"

echo ""
echo "[验证] ELF 镜像结构..."
if command -v aarch64-elf-readelf &>/dev/null; then
    READELF=aarch64-elf-readelf
elif command -v readelf &>/dev/null; then
    READELF=readelf
else
    READELF=""
fi

if [ -n "$READELF" ]; then
    ENTRY_POINT=$($READELF -h "$IMAGE" 2>/dev/null | grep "Entry point" | awk '{print $NF}')
    MACHINE=$($READELF -h "$IMAGE" 2>/dev/null | grep "Machine:" | sed 's/.*Machine: *//')
    echo "  Machine:     $MACHINE"
    echo "  Entry point: $ENTRY_POINT"

    echo ""
    echo "[验证] 包含的 LOAD 段:"
    $READELF -l "$IMAGE" 2>/dev/null | grep -E "LOAD|Entry" | head -10
else
    echo "  (readelf 不可用, 跳过 ELF 验证)"
    file "$IMAGE"
fi

# 检测平台配置
echo ""
echo "[平台] L4Re 配置:"
PLATFORM_TYPE=$(grep "^CONFIG_PLATFORM_TYPE=" "$KCONFIG" 2>/dev/null | cut -d'"' -f2)
RAM_BASE=$(grep "PLATFORM_RAM_BASE" "$BUILD_DIR/.config.all" 2>/dev/null | head -1 | sed 's/.*= *//')
echo "  PLATFORM_TYPE: ${PLATFORM_TYPE:-unknown}"
echo "  RAM_BASE:      ${RAM_BASE:-unknown}"

# 检测内核配置
KERNEL_BUILD="$PROJ_ROOT/kernel/build"
if [ -f "$KERNEL_BUILD/globalconfig.out" ]; then
    KERNEL_PLATFORM=$(grep "^CONFIG_PF_" "$KERNEL_BUILD/globalconfig.out" 2>/dev/null | grep "=y" | head -1 | sed 's/CONFIG_PF_//;s/=y//')
    echo "  Kernel PF:     ${KERNEL_PLATFORM:-unknown}"
fi

# 平台兼容性检查
echo ""
QEMU_MACHINE=""
QEMU_EXTRA=""

case "$PLATFORM_TYPE" in
    arm_virt)
        QEMU_MACHINE="virt,virtualization=true"
        QEMU_EXTRA="-cpu cortex-a57 -m 1024"
        echo "[平台] arm_virt -> QEMU: -M virt (兼容)"
        ;;
    rv_vexpress_a15)
        QEMU_MACHINE="virt,virtualization=true"
        QEMU_EXTRA="-cpu cortex-a57 -m 1024"
        echo "[警告] rv_vexpress_a15 与 QEMU virt 机器不完全兼容!"
        echo "  rv_vexpress_a15 RAM 起始地址: 0x80000000"
        echo "  QEMU virt RAM 起始地址:      0x40000000"
        echo "  建议: 将 L4Re 重新配置为 arm_virt 平台以使用 QEMU 测试"
        echo "    cd $BUILD_DIR && CROSS_COMPILE=aarch64-elf- make oldconfig"
        ;;
    *)
        QEMU_MACHINE="virt"
        QEMU_EXTRA="-cpu cortex-a57 -m 1024"
        echo "[警告] 未知平台 '$PLATFORM_TYPE', 使用默认 QEMU virt"
        ;;
esac

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo ""
    echo "[完成] 镜像验证结束 (--check 模式, 不启动 QEMU)"
    exit 0
fi

# ============================================================
# QEMU 启动
# ============================================================
QEMU=qemu-system-aarch64
if ! command -v "$QEMU" &>/dev/null; then
    echo "Error: 找不到 $QEMU"
    echo "  macOS: brew install qemu"
    exit 1
fi

QEMU_OPTS=(
    -M "$QEMU_MACHINE"
    $QEMU_EXTRA
    -nographic
    -kernel "$IMAGE"
)

echo ""
echo "[QEMU] $QEMU ${QEMU_OPTS[*]}"

if [ "$TIMEOUT" -gt 0 ] 2>/dev/null; then
    echo "[QEMU] 超时: ${TIMEOUT}s"
    echo "--- 输出 ---"
    timeout "$TIMEOUT" "$QEMU" "${QEMU_OPTS[@]}" || EXIT_CODE=$?

    echo ""
    echo "--- QEMU 退出 (code=${EXIT_CODE:-0}) ---"

    if [ "${EXIT_CODE:-0}" -eq 124 ]; then
        echo "QEMU 因超时而终止 (这是正常的)"
        exit 0
    elif [ "${EXIT_CODE:-0}" -ne 0 ]; then
        echo "QEMU 异常退出"
        exit 1
    fi
else
    echo "按 Ctrl-A X 退出 QEMU"
    echo "--- 输出 ---"
    "$QEMU" "${QEMU_OPTS[@]}"
fi
