#!/bin/bash
# TuringOS 构建脚本
# 用法:
#   ./build.sh              # 完整构建 (内核 + L4Re + 引导镜像)
#   ./build.sh kernel       # 仅构建内核
#   ./build.sh l4re         # 仅构建 L4Re
#   ./build.sh bootstrap    # 仅生成引导镜像
#   ./build.sh menuconfig   # 交互式驱动配置菜单
#   ./build.sh clean        # 清理所有构建产物

set -e

# macOS 自带 Make 3.81 太旧，优先使用 Homebrew 的 GNU Make 4+
if command -v gmake &>/dev/null; then
    MAKE=gmake
else
    MAKE=make
fi

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="$PROJ_ROOT/kernel"
KERNEL_BUILD="$KERNEL_DIR/build"
KERNEL_TEMPLATE="arm64-rpi4"
L4RE_DIR="$PROJ_ROOT/l4re"
L4RE_BUILD="$L4RE_DIR/build_arm64"
CONF_DIR="$PROJ_ROOT/conf"
BUILD_OUT="$PROJ_ROOT/build"
export CROSS_COMPILE="${CROSS_COMPILE:-aarch64-elf-}"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}>>> $1${NC}"; }
warn()  { echo -e "${YELLOW}!!! $1${NC}"; }
error() { echo -e "${RED}*** $1${NC}"; exit 1; }

# ============================================================
# 检查构建依赖
# ============================================================
check_deps() {
    info "检查构建依赖..."

    # 检查交叉编译工具链
    if ! command -v "${CROSS_COMPILE}gcc" &>/dev/null; then
        error "找不到交叉编译器: ${CROSS_COMPILE}gcc
请安装 aarch64 工具链:
  macOS:   brew install aarch64-elf-gcc  (或使用 aarch64-linux-gnu- 前缀的工具链)
  Ubuntu:  sudo apt install gcc-aarch64-linux-gnu
  Fedora:  sudo dnf install gcc-aarch64-linux-gnu
或设置 CROSS_COMPILE 环境变量指向你的工具链前缀:
  export CROSS_COMPILE=aarch64-none-elf-"
    fi

    # 检查 make
    if ! command -v $MAKE &>/dev/null; then
        error "找不到 $MAKE，请先安装 GNU Make 4+"
    fi

    # 检查 perl (内核构建需要)
    if ! command -v perl &>/dev/null; then
        error "找不到 perl，内核构建需要它"
    fi

    # 检查 qemu (可选，仅用于运行)
    if ! command -v qemu-system-aarch64 &>/dev/null; then
        warn "找不到 qemu-system-aarch64，构建可以继续但无法运行"
    fi

    # 检查 flex/bison (kconfig 工具构建需要)
    if ! command -v flex &>/dev/null; then
        warn "找不到 flex，menuconfig 功能需要它"
    fi
    if ! command -v bison &>/dev/null; then
        warn "找不到 bison，menuconfig 功能需要它"
    fi

    info "依赖检查通过"
}

# ============================================================
# 初始化 Git 子模块
# ============================================================
init_submodules() {
    info "检查 Git 子模块..."
    cd "$PROJ_ROOT"

    if [ ! -f "$KERNEL_DIR/Makefile" ] || [ ! -f "$L4RE_DIR/Makefile" ]; then
        info "初始化 Git 子模块..."
        git submodule update --init --recursive
    fi
}

# ============================================================
# 构建 Fiasco 内核
# ============================================================
build_kernel() {
    info "构建 Fiasco 内核 (模板: $KERNEL_TEMPLATE)..."
    cd "$KERNEL_DIR"

    # 检查 build 目录的 Makefile 中 srcdir 是否正确
    local need_rebuild=0
    if [ -f "$KERNEL_BUILD/Makefile" ]; then
        if ! grep -qF "srcdir  := $KERNEL_DIR/src" "$KERNEL_BUILD/Makefile"; then
            warn "内核构建目录的源码路径过期，将重新创建"
            need_rebuild=1
        fi
    else
        need_rebuild=1
    fi

    if [ "$need_rebuild" -eq 1 ]; then
        rm -rf "$KERNEL_BUILD"
        info "创建内核构建目录 (模板: $KERNEL_TEMPLATE)..."
        $MAKE BUILDDIR=build T="$KERNEL_TEMPLATE"
    fi

    # 编译内核
    info "编译内核..."
    $MAKE -C "$KERNEL_BUILD" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

    # 复制内核二进制到输出目录
    mkdir -p "$BUILD_OUT/kernel"
    if [ -f "$KERNEL_BUILD/fiasco" ]; then
        cp "$KERNEL_BUILD/fiasco" "$BUILD_OUT/kernel/"
        info "内核已编译: $BUILD_OUT/kernel/fiasco"
    else
        error "内核编译失败: 找不到 fiasco 二进制文件"
    fi
}

# ============================================================
# 构建 L4Re 运行时环境
# ============================================================
build_l4re() {
    info "构建 L4Re 运行时环境..."

    local l4mk_dir="$PROJ_ROOT/l4mk"

    if [ ! -d "$L4RE_BUILD" ]; then
        info "创建 L4Re 构建目录 (模板: arm64-rv-v8a)..."
        $MAKE -C "$l4mk_dir" B="$L4RE_BUILD" T=arm64-rv-v8a
    fi

    cd "$L4RE_BUILD"
    $MAKE -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    info "L4Re 构建完成"
}

# ============================================================
# 生成引导镜像 (bootstrap.elf)
# ============================================================
build_bootstrap() {
    info "生成引导镜像..."

    if [ ! -d "$L4RE_BUILD" ]; then
        error "L4Re 构建目录不存在，请先构建 L4Re"
    fi

    if [ ! -f "$BUILD_OUT/kernel/fiasco" ]; then
        error "内核尚未编译，请先构建内核"
    fi

    # 设置模块搜索路径: 内核输出 + 配置文件
    export MODULE_SEARCH_PATH="$BUILD_OUT/kernel:$CONF_DIR"

    cd "$L4RE_BUILD"
    $MAKE E=fiasco-base-test elfimage

    # 复制引导镜像到输出目录
    if [ -f "$L4RE_BUILD/images/bootstrap.elf" ]; then
        cp "$L4RE_BUILD/images/bootstrap.elf" "$BUILD_OUT/"
        info "引导镜像已生成: $BUILD_OUT/bootstrap.elf"
        info "可使用 ./run_qemu.sh 启动系统"
    else
        error "引导镜像生成失败"
    fi
}

# ============================================================
# 清理构建产物
# ============================================================
do_clean() {
    info "清理构建产物..."

    # 清理内核
    if [ -d "$KERNEL_BUILD" ]; then
        $MAKE -C "$KERNEL_BUILD" clean 2>/dev/null || true
    fi

    # 清理 L4Re
    if [ -d "$L4RE_BUILD" ]; then
        $MAKE -C "$L4RE_BUILD" clean 2>/dev/null || true
    fi

    # 清理输出目录
    rm -rf "$BUILD_OUT/kernel/fiasco"
    rm -f "$BUILD_OUT/bootstrap.elf"

    info "清理完成"
}

# ============================================================
# 驱动配置
# ============================================================
check_config() {
    if [ ! -f "$PROJ_ROOT/.config" ]; then
        warn "驱动配置文件 (.config) 不存在"
        info "使用默认配置 (defconfig) ..."
        $MAKE -C "$PROJ_ROOT" defconfig
    fi
}

# 将配置同步到 L4Re 构建目录
sync_config_to_l4re() {
    local auto_conf="$PROJ_ROOT/include/config/auto.conf"
    local autoconf_h="$PROJ_ROOT/include/generated/autoconf.h"

    # 确保 auto.conf 存在 (syncconfig 已在 defconfig/menuconfig 中自动执行)
    if [ ! -f "$auto_conf" ]; then
        info "生成 auto.conf ..."
        $MAKE -C "$PROJ_ROOT" syncconfig
    fi

    # 如果 L4Re 构建目录存在，复制配置文件进去
    if [ -d "$L4RE_BUILD" ]; then
        info "同步驱动配置到 L4Re 构建目录..."
        mkdir -p "$L4RE_BUILD/include/config"
        mkdir -p "$L4RE_BUILD/include/generated"
        cp "$auto_conf" "$L4RE_BUILD/include/config/auto.conf"
        cp "$autoconf_h" "$L4RE_BUILD/include/generated/autoconf.h"

        # 复制依赖跟踪用的空文件 (用于 make 增量构建)
        if [ -d "$PROJ_ROOT/include/config" ]; then
            find "$PROJ_ROOT/include/config" -maxdepth 1 -type f ! -name 'auto.conf*' \
                -exec cp {} "$L4RE_BUILD/include/config/" \;
        fi
        info "驱动配置已同步"
    fi
}

do_menuconfig() {
    info "启动驱动配置菜单..."
    $MAKE -C "$PROJ_ROOT" menuconfig
}

# ============================================================
# 主流程
# ============================================================
main() {
    local target="${1:-all}"

    echo "========================================"
    echo "  TuringOS 构建系统"
    echo "  目标平台: ARM64 (Raspberry Pi 4B)"
    echo "  交叉编译器: ${CROSS_COMPILE}gcc"
    echo "========================================"

    case "$target" in
        all)
            check_deps
            init_submodules
            check_config
            sync_config_to_l4re
            build_kernel
            build_l4re
            build_bootstrap
            ;;
        kernel)
            check_deps
            init_submodules
            build_kernel
            ;;
        l4re)
            check_deps
            check_config
            sync_config_to_l4re
            build_l4re
            ;;
        bootstrap)
            build_bootstrap
            ;;
        menuconfig)
            do_menuconfig
            ;;
        defconfig)
            $MAKE -C "$PROJ_ROOT" defconfig
            ;;
        clean)
            do_clean
            ;;
        *)
            echo "用法: $0 {all|kernel|l4re|bootstrap|menuconfig|defconfig|clean}"
            echo ""
            echo "  all         完整构建 (内核 + L4Re + 引导镜像)"
            echo "  kernel      仅构建 Fiasco 内核"
            echo "  l4re        仅构建 L4Re 运行时"
            echo "  bootstrap   仅生成引导镜像 (需先完成 kernel 和 l4re)"
            echo "  menuconfig  交互式驱动配置菜单"
            echo "  defconfig   使用默认 RPi4 驱动配置"
            echo "  clean       清理所有构建产物"
            echo ""
            echo "环境变量:"
            echo "  CROSS_COMPILE  交叉编译器前缀 (默认: aarch64-linux-gnu-)"
            exit 1
            ;;
    esac

    info "构建完成!"
}

main "$@"
