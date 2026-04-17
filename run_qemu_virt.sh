#!/bin/bash
# QEMU 启动脚本 - ARM Virt Platform

echo "=========================================="
echo "  TuringOS QEMU 测试 - Virt Platform"
echo "=========================================="

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"

# ---- 命令行参数解析 ----
NET_MODE=""    # "" = 无网络, "tcp" = TCP server 模式, "shell" = net-shell 模式
HOST_PORT=5555 # 主机侧端口，转发到 guest:5000

while [[ $# -gt 0 ]]; do
    case "$1" in
        --net-tcp)
            NET_MODE="tcp"
            shift
            ;;
        --net-shell)
            NET_MODE="shell"
            shift
            ;;
        --host-port)
            HOST_PORT="$2"
            shift 2
            ;;
        --help|-h)
            echo ""
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --net-tcp         启用网络，启动 tcp-server 镜像"
            echo "  --net-shell       启用网络，启动 net-shell 镜像 (shell 中输入 'net' 启动服务)"
            echo "  --host-port PORT  主机转发端口 (默认 5555)"
            echo ""
            echo "示例:"
            echo "  $0                     # 无网络，标准启动"
            echo "  $0 --net-shell         # 带网络的 shell，输入 'net' 启动 TCP server"
            echo "  $0 --net-tcp           # 直接启动 TCP echo server"
            echo "  $0 --net-shell --host-port 8080"
            echo ""
            echo "TCP server 测试:"
            echo "  1. 先构建: make -C build/l4re_virt PKGS=native_shell"
            echo "  2. 启动:   $0 --net-shell"
            echo "  3. 在 shell 中输入: net"
            echo "  4. 在另一终端: python3 tools/tcp_client.py --port $HOST_PORT"
            exit 0
            ;;
        *)
            echo "未知参数: $1 (使用 --help 查看帮助)"
            shift
            ;;
    esac
done

# ---- 查找镜像 ----
if [ "$NET_MODE" = "shell" ]; then
    BUILDS=(
        "$PROJ_ROOT/build/l4re_virt/images/bootstrap_native-shell.elf"
        "$PROJ_ROOT/build/l4re_virt/images/bootstrap.elf"
        "$PROJ_ROOT/build/l4re_arm64/images/bootstrap.elf"
        "$PROJ_ROOT/build/artifacts/bootstrap-image-virt.elf"
        "$PROJ_ROOT/build/artifacts/bootstrap-final-rpi4.elf"
    )
elif [ "$NET_MODE" = "tcp" ]; then
    BUILDS=(
        "$PROJ_ROOT/build/l4re_virt/images/bootstrap_tcp-server.elf"
        "$PROJ_ROOT/build/l4re_virt/images/bootstrap_native-shell.elf"
        "$PROJ_ROOT/build/l4re_virt/images/bootstrap.elf"
        "$PROJ_ROOT/build/l4re_arm64/images/bootstrap.elf"
        "$PROJ_ROOT/build/artifacts/bootstrap-image-virt.elf"
        "$PROJ_ROOT/build/artifacts/bootstrap-final-rpi4.elf"
    )
else
    BUILDS=(
        "$PROJ_ROOT/build/l4re_virt/images/bootstrap_native-shell.elf"
        "$PROJ_ROOT/build/l4re_virt/images/bootstrap.elf"
        "$PROJ_ROOT/build/l4re_virt/images/bootstrap_tcp-server.elf"
        "$PROJ_ROOT/build/l4re_arm64/images/bootstrap.elf"
        "$PROJ_ROOT/build/artifacts/bootstrap-image-virt.elf"
        "$PROJ_ROOT/build/artifacts/bootstrap-final-rpi4.elf"
    )
fi

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
    echo "  make -C build/l4re_virt PKGS=hello"
    exit 1
fi

# ---- 检测镜像架构 ----
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
    MACHINE_TYPE="aarch64"
    QEMU_ARCH="qemu-system-aarch64"
    CPU="-cpu cortex-a57"
    MEM="-m 1024M"
    MACHINE="-M virt,virtualization=true"
fi

# ---- 网络参数 ----
NET_ARGS=""
if [ "$NET_MODE" = "tcp" ] || [ "$NET_MODE" = "shell" ]; then
    NET_ARGS="-netdev user,id=net0,hostfwd=tcp::${HOST_PORT}-:5000 -device virtio-net-device,netdev=net0"
    echo ""
    echo "网络模式: 用户 NAT (slirp)"
    echo "  客户机 IP : 10.0.2.15"
    echo "  客户机端口: 5000 (TCP echo server)"
    echo "  主机转发  : localhost:${HOST_PORT} → 客户机:5000"
    echo ""
    if [ "$NET_MODE" = "shell" ]; then
        echo "启动后在 shell 中输入 'net' 启动 TCP echo server"
    fi
    echo "测试命令 (在另一终端):"
    echo "  python3 tools/tcp_client.py --port ${HOST_PORT}"
fi

echo ""
echo "使用架构: $MACHINE_TYPE"
echo "镜像路径: $IMAGE"
echo ""
echo "启动 L4Re 系统..."
echo "按 Ctrl-A X 退出 QEMU"
echo "------------------------------------------"

# shellcheck disable=SC2086
$QEMU_ARCH \
    $MACHINE \
    $CPU \
    $MEM \
    -kernel "$IMAGE" \
    -nographic \
    -serial mon:stdio \
    $NET_ARGS
