#!/bin/bash
# QEMU 启动脚本 - ARM Virt Platform

echo "=========================================="
echo "  TuringOS QEMU 测试 - Virt Platform"
echo "=========================================="

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"

# ---- 命令行参数解析 ----
NET_MODE="shell" # 默认启用网络
HOST_PORT=5555   # 主机侧端口，转发到 guest:5000
GPU_MODE=""      # GPU 模式：启用 ramfb + 显示输出
VNC_PORT=5900    # VNC 端口（--gpu --vnc 时使用）
CFG_NAME=""      # 指定配置名称 (--cfg <name> → bootstrap_<name>.elf)
GPU_DISPLAY="gtk" # 显示后端：gtk（默认）/ vnc
FB_RES="1280x720" # 帧缓冲分辨率
SMP_CPUS=2       # 默认双核（Fiasco CONFIG_MP=y，最多16核）
DISK_SIZE="100M" # 虚拟磁盘大小（设为空字符串禁用磁盘）

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-net)
            NET_MODE=""
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
        --gpu)
            GPU_MODE="ramfb"
            GPU_DISPLAY="${GPU_DISPLAY:-vnc}"
            shift
            ;;
        --vnc)
            GPU_DISPLAY="vnc"
            if [[ "$2" =~ ^[0-9]+$ ]]; then
                VNC_PORT="$2"
                shift
            fi
            shift
            ;;
        --gtk)
            GPU_DISPLAY="gtk"
            shift
            ;;
        --fb-res)
            FB_RES="$2"
            shift 2
            ;;
        --smp)
            SMP_CPUS="$2"
            shift 2
            ;;
        --cfg)
            CFG_NAME="$2"; shift 2 ;;
        --no-smp)
            SMP_CPUS=1
            shift
            ;;
        --disk)
            DISK_SIZE="$2"
            shift 2
            ;;
        --no-disk)
            DISK_SIZE=""
            shift
            ;;
        --help|-h)
            echo ""
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --cfg NAME            指定配置 (加载 bootstrap_NAME.elf，如 smp-test, fb-test)
  --no-net              禁用网络"
            echo "  --net-shell           启用网络 (默认已启用)"
            echo "  --host-port PORT      主机转发端口 (默认 5555)"
            echo "  --gpu                 启用 GPU 调试 (ramfb + GTK 窗口，默认)"
            echo "  --vnc [PORT]          VNC 显示，可选端口 (默认 5900)"
            echo "  --gtk                 GTK 窗口显示 (需要本地桌面环境)"
            echo "  --fb-res WxH          帧缓冲分辨率 (默认 1280x720)"
            echo "  --smp N               CPU 核心数 (默认 2，Fiasco 最多 16)"
            echo "  --no-smp              单核模式"
            echo "  --disk SIZE           虚拟磁盘大小 (默认 100M，如 512M, 1G)"
            echo "  --no-disk             禁用虚拟磁盘"
            echo ""
            echo "示例:"
            echo "  $0                         # 带网络启动 (默认)"
            echo "  $0 --no-net                # 不带网络启动"
            echo "  $0 --host-port 8080        # 自定义转发端口"
            echo "  $0 --disk 512M             # 512MB 虚拟磁盘"
            echo "  $0 --no-disk               # 禁用虚拟磁盘"
            echo "  $0 --gpu                   # GPU 调试，VNC 监听 :5900"
            echo "  $0 --gpu --vnc 5901        # GPU 调试，VNC 监听 :5901"
            echo "  $0 --gpu --gtk             # GPU 调试，GTK 窗口"
            echo "  $0 --gpu --fb-res 1920x1080 # 1080p 帧缓冲"
            echo ""
            echo "GPU 调试说明:"
            echo "  --gpu 模式启用 QEMU ramfb 设备，bootstrap 自动配置帧缓冲。"
            echo "  L4Re 通过 'vesa' capability 访问帧缓冲 (由 MOE 从 VBE 信息注册)。"
            echo "  使用 conf/fb-test.cfg 运行帧缓冲测试:"
            echo "    ./build.sh --board virt all"
            echo "    $0 --gpu  (另一终端连接 VNC: vncviewer localhost:5900)"
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
if [ -n "$CFG_NAME" ]; then
    BUILDS=("$PROJ_ROOT/build/l4re_virt/images/bootstrap_${CFG_NAME}.elf")
else
    BUILDS=(
        "$PROJ_ROOT/build/l4re_virt/images/bootstrap_native-shell.elf"
        "$PROJ_ROOT/build/l4re_virt/images/bootstrap.elf"
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
    echo "  make -C build/l4re_virt PKGS=native_shell"
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
if [ "$NET_MODE" = "shell" ]; then
    NET_ARGS="-netdev user,id=net0,hostfwd=tcp::${HOST_PORT}-:5000 -device virtio-net-device,netdev=net0"
    echo ""
    echo "网络模式: 用户 NAT (slirp)"
    echo "  客户机 IP : 10.0.2.15"
    echo "  客户机端口: 5000 (TCP echo server)"
    echo "  主机转发  : localhost:${HOST_PORT} → 客户机:5000"
    echo ""
    echo "启动后在 shell 中输入 'net' 启动 TCP echo server"
    echo "测试命令 (在另一终端):"
    echo "  python3 tools/tcp_client.py --port ${HOST_PORT}"
fi

# ---- GPU / 显示参数 ----
GPU_ARGS=""
DISPLAY_ARGS="-nographic"
SERIAL_ARGS="-serial mon:stdio"

if [ -n "$GPU_MODE" ]; then
    # ramfb: QEMU 将帧缓冲暴露给 fw_cfg，bootstrap 自动检测并初始化
    # 分辨率通过 fw_cfg "opt/org.l4re/fb_res" 传递
    GPU_ARGS="-device ramfb -fw_cfg name=opt/org.l4re/fb_res,string=${FB_RES}"

    case "$GPU_DISPLAY" in
        vnc)
            VNC_IDX=$(( VNC_PORT - 5900 ))
            DISPLAY_ARGS="-display vnc=127.0.0.1:${VNC_IDX}"
            echo ""
            echo "GPU 模式: ramfb + VNC"
            echo "  分辨率 : ${FB_RES}"
            echo "  VNC 端口: ${VNC_PORT} (显示 :${VNC_IDX})"
            echo "  连接命令: vncviewer localhost:${VNC_PORT}"
            echo "           或 vncviewer localhost::${VNC_PORT}"
            ;;
        gtk)
            DISPLAY_ARGS="-display gtk"
            echo ""
            echo "GPU 模式: ramfb + GTK 窗口"
            echo "  分辨率: ${FB_RES}"
            ;;
        *)
            DISPLAY_ARGS="-display gtk"
            echo ""
            echo "GPU 模式: ramfb + GTK 窗口 (默认)"
            ;;
    esac

    # GPU 模式下 serial 单独走 stdio（不用 mon:stdio 以免干扰显示）
    SERIAL_ARGS="-serial stdio -monitor none"

    echo "  启动配置: 使用 conf/fb-test.cfg 测试帧缓冲"
fi

# ---- 磁盘参数 ----
DISK_ARGS=""
DISK_IMAGE="$PROJ_ROOT/build/virt_disk.img"

if [ -n "$DISK_SIZE" ]; then
    # 创建虚拟磁盘（如果不存在）
    if [ ! -f "$DISK_IMAGE" ]; then
        echo ""
        echo "创建虚拟磁盘: $DISK_IMAGE ($DISK_SIZE)"
        mkdir -p "$(dirname "$DISK_IMAGE")"
        qemu-img create -f raw "$DISK_IMAGE" "$DISK_SIZE"
        echo "虚拟磁盘创建完成"
    fi

    # 添加 VirtIO 块设备
    DISK_ARGS="-drive if=virtio,file=$DISK_IMAGE,format=raw"
    echo ""
    echo "磁盘设备: VirtIO Block"
    echo "  磁盘路径: $DISK_IMAGE"
    echo "  磁盘大小: $DISK_SIZE"
fi

echo ""
echo "使用架构: $MACHINE_TYPE"
echo "镜像路径: $IMAGE"
echo ""
echo "启动 L4Re 系统..."
if [ -n "$GPU_MODE" ]; then
    echo "按 Ctrl-C 退出 QEMU"
else
    echo "按 Ctrl-A X 退出 QEMU"
fi
echo "CPU 核心数: $SMP_CPUS"
echo "------------------------------------------"

# shellcheck disable=SC2086
$QEMU_ARCH \
    $MACHINE \
    $CPU \
    $MEM \
    -smp "$SMP_CPUS" \
    -kernel "$IMAGE" \
    $DISPLAY_ARGS \
    $SERIAL_ARGS \
    $GPU_ARGS \
    $NET_ARGS \
    $DISK_ARGS
