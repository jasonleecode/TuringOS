#!/bin/bash
# TuringOS QEMU 启动测试脚本
# 用法:
#   ./run_qemu.sh                     # 启动默认镜像 (hello, RPi4)
#   ./run_qemu.sh --board bbb --check # 验证 BBB 镜像
#   ./run_qemu.sh <entry>             # 启动指定 entry 的镜像
#   ./run_qemu.sh --timeout 5         # 设置超时秒数 (0=不限)
#   ./run_qemu.sh --check             # 仅验证镜像, 不启动 QEMU

set -e

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"

# ============================================================
# 解析参数
# ============================================================
BOARD=""
ENTRY="hello"
TIMEOUT=10
CHECK_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --board)   BOARD="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --check)   CHECK_ONLY=1; shift ;;
        *)         ENTRY="$1"; shift ;;
    esac
done

# ============================================================
# 根据目标板设置路径
# ============================================================
ARTIFACTS_DIR="$PROJ_ROOT/build/artifacts"
case "$BOARD" in
    bbb)
        BOARD_NAME="BeagleBone Black (AM335x)"
        BUILD_DIR="$PROJ_ROOT/build/l4re_bbb"
        KERNEL_BUILD="$PROJ_ROOT/build/kernel_bbb"
        CROSS_COMPILE="arm-linux-gnueabihf-"
        IMAGE="$ARTIFACTS_DIR/bootstrap-final-bbb.elf"
        ;;
    *)
        BOARD_NAME="Raspberry Pi 4B"
        BUILD_DIR="$PROJ_ROOT/build/l4re_arm64"
        KERNEL_BUILD="$PROJ_ROOT/build/kernel_arm64"
        CROSS_COMPILE="aarch64-elf-"
        IMAGE="$ARTIFACTS_DIR/bootstrap-final-rpi4.elf"
        ;;
esac

# 如果 artifacts 目录没有，尝试传统的 images 目录
if [ ! -f "$IMAGE" ]; then
    IMAGES_DIR="$BUILD_DIR/images"
    IMAGE="$IMAGES_DIR/bootstrap_${ENTRY}.elf"
    if [ ! -f "$IMAGE" ]; then
        IMAGE="$IMAGES_DIR/bootstrap.elf"
    fi
fi

if [ ! -f "$IMAGE" ]; then
    echo "Error: 找不到引导镜像"
    echo "  尝试过: $ARTIFACTS_DIR/bootstrap-final-${BOARD:-rpi4}.elf"
    echo "  尝试过: $BUILD_DIR/images/bootstrap_${ENTRY}.elf"
    echo "  尝试过: $BUILD_DIR/images/bootstrap.elf"
    echo ""
    echo "请先运行: ./build.sh${BOARD:+ --board $BOARD}"
    exit 1
fi

# ============================================================
# 镜像验证
# ============================================================
echo "========================================"
echo "  TuringOS 启动测试"
echo "  目标板: $BOARD_NAME"
echo "  镜像: $(basename "$IMAGE") ($(du -h "$IMAGE" | cut -f1))"
echo "========================================"

echo ""
echo "[验证] ELF 镜像结构..."
if command -v ${CROSS_COMPILE}readelf &>/dev/null; then
    READELF=${CROSS_COMPILE}readelf
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
PLATFORM_TYPE=$(grep "^CONFIG_PLATFORM_TYPE=" "$BUILD_DIR/.kconfig" 2>/dev/null | cut -d'"' -f2)
RAM_BASE=$(grep "PLATFORM_RAM_BASE" "$BUILD_DIR/.config.all" 2>/dev/null | head -1 | sed 's/.*= *//')
echo "  PLATFORM_TYPE: ${PLATFORM_TYPE:-unknown}"
echo "  RAM_BASE:      ${RAM_BASE:-unknown}"

# 检测内核配置
if [ -f "$KERNEL_BUILD/globalconfig.out" ]; then
    KERNEL_PLATFORM=$(grep "^CONFIG_PF_" "$KERNEL_BUILD/globalconfig.out" 2>/dev/null | grep "=y" | head -1 | sed 's/CONFIG_PF_//;s/=y//')
    echo "  Kernel PF:     ${KERNEL_PLATFORM:-unknown}"
fi

# ============================================================
# 平台兼容性检查
# ============================================================
echo ""
QEMU_MACHINE=""
QEMU_EXTRA=""
QEMU=""

case "$PLATFORM_TYPE" in
    arm_virt)
        QEMU=qemu-system-aarch64
        QEMU_MACHINE="virt,virtualization=true"
        QEMU_EXTRA="-cpu cortex-a57 -m 1024"
        echo "[平台] arm_virt -> QEMU: -M virt (兼容)"
        ;;
    rv_vexpress_a15)
        QEMU=qemu-system-aarch64
        QEMU_MACHINE="virt,virtualization=true"
        QEMU_EXTRA="-cpu cortex-a57 -m 1024"
        echo "[警告] rv_vexpress_a15 与 QEMU virt 机器不完全兼容!"
        echo "  rv_vexpress_a15 RAM 起始地址: 0x80000000"
        echo "  QEMU virt RAM 起始地址:      0x40000000"
        echo "  建议: 将 L4Re 重新配置为 arm_virt 平台以使用 QEMU 测试"
        ;;
    omap3_am33xx)
        echo "[平台] omap3_am33xx (BeagleBone Black / AM335x)"
        echo "  QEMU 不支持 AM335x SoC，无法在模拟器中运行。"
        echo ""
        echo "  真机部署方法:"
        echo "    1. 生成 uImage:  ${CROSS_COMPILE}objcopy -O binary $IMAGE bootstrap.bin"
        echo "       mkimage -A arm -T kernel -C none -a 0x80000000 -e 0x80000000 -d bootstrap.bin uImage"
        echo "    2. 将 uImage 复制到 SD 卡 FAT 分区"
        echo "    3. U-Boot 中执行: fatload mmc 0 0x80000000 uImage && bootm 0x80000000"
        if [ "$CHECK_ONLY" -eq 1 ]; then
            echo ""
            echo "[完成] 镜像验证结束 (--check 模式)"
            exit 0
        fi
        echo ""
        echo "[错误] 无法启动 QEMU: AM335x 平台不支持 QEMU 模拟"
        exit 1
        ;;
    *)
        QEMU=qemu-system-aarch64
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
