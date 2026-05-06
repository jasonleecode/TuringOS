#!/bin/bash
# QEMU 启动脚本 - i.MX6UL (mcimx6ul-evk)
#
# 机器参数：
#   CPU:  Cortex-A7
#   RAM:  512M (@ 0x80000000)
#   UART: UART1 @ 0x02020000 → 串口控制台
#
# 用法:
#   ./run_qemu_imx6ul.sh
#   ./run_qemu_imx6ul.sh --cfg imx6ul-native-shell
#   ./run_qemu_imx6ul.sh --timeout 30
#   ./run_qemu_imx6ul.sh --help

set -e

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"
CFG_NAME="imx6ul-native-shell"
TIMEOUT=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cfg)
            CFG_NAME="$2"; shift 2 ;;
        --timeout)
            TIMEOUT="$2"; shift 2 ;;
        --check)
            # 仅验证镜像结构，不启动 QEMU
            IMAGE="$PROJ_ROOT/build/l4re_imx6ul/images/bootstrap_${CFG_NAME}.elf"
            if [ -f "$IMAGE" ]; then
                echo "镜像验证通过: $IMAGE"
                file "$IMAGE"
                exit 0
            else
                echo "错误: 镜像不存在: $IMAGE"
                exit 1
            fi
            ;;
        --help|-h)
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --cfg NAME      启动配置 (默认: imx6ul-native-shell)"
            echo "  --timeout SEC   超时秒数 (0=不限，默认: 0)"
            echo "  --check         仅验证镜像，不启动 QEMU"
            echo ""
            echo "退出: Ctrl-A X"
            exit 0
            ;;
        *)
            echo "未知参数: $1 (使用 --help 查看帮助)"
            shift ;;
    esac
done

IMAGE="$PROJ_ROOT/build/l4re_imx6ul/images/bootstrap_${CFG_NAME}.elf"

if [ ! -f "$IMAGE" ]; then
    echo "错误: 找不到镜像: $IMAGE"
    echo ""
    echo "请先构建:"
    echo "  ./build.sh --board imx6ul all"
    exit 1
fi

echo "=========================================="
echo "  TuringOS QEMU 测试 - i.MX6UL"
echo "=========================================="
echo "镜像: $IMAGE"
echo "机器: mcimx6ul-evk (Cortex-A7)"
echo "控制台: UART1 @ 0x02020000"
echo "退出: Ctrl-A X"
echo "------------------------------------------"

QEMU_CMD=(
    qemu-system-arm
    -M mcimx6ul-evk
    -cpu cortex-a7
    -m 512
    -nographic
    -serial mon:stdio
    -kernel "$IMAGE"
)

if [ "$TIMEOUT" -gt 0 ] 2>/dev/null; then
    exec timeout "$TIMEOUT" "${QEMU_CMD[@]}"
else
    exec "${QEMU_CMD[@]}"
fi
