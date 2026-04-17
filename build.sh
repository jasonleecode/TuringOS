#!/bin/bash
# TuringOS 构建脚本
#
# 用法:
#   ./build.sh [--board <board>] <target>
#
# 目标板 (--board):
#   rpi4   - Raspberry Pi 4B, ARM64 (默认)
#   bbb    - BeagleBone Black, AM335x ARM (真实硬件)
#   virt   - QEMU ARM Virt, Cortex-A15 (开发测试推荐)
#
# 构建目标:
#   all        - 完整构建 (内核 + L4Re + 引导镜像 + 收集产物)
#   kernel     - 仅构建 Fiasco 内核
#   l4re       - 仅构建 L4Re 运行时
#   bootstrap  - 仅生成引导镜像 (需先完成 kernel 和 l4re)
#   collect    - 收集构建产物到 build/artifacts 目录
#   clean      - 清理编译中间文件
#   cleanall   - 完全清理构建目录
#
# 示例:
#   ./build.sh                      # 默认 RPi4 全量构建
#   ./build.sh --board virt all     # QEMU virt 全量构建
#   ./build.sh --board bbb kernel   # BeagleBone Black 构建内核
#   ./build.sh --board virt collect # 仅收集 virt 构建产物
#
# 快速开始 (QEMU 测试):
#   ./build.sh --board virt all     # 构建
#   ./run_qemu_virt.sh              # 运行

set -e

# macOS 自带 Make 3.81 太旧，优先使用 Homebrew 的 GNU Make 4+
if command -v gmake &>/dev/null; then
    MAKE=gmake
else
    MAKE=make
fi

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="$PROJ_ROOT/kernel"
CONF_DIR="$PROJ_ROOT/conf"
BUILD_OUT="$PROJ_ROOT/build"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}>>> $1${NC}"; }
warn()  { echo -e "${YELLOW}!!! $1${NC}"; }
error() { echo -e "${RED}*** $1${NC}"; exit 1; }

# ============================================================
# 目标板配置
# ============================================================
setup_board() {
    local board="$1"
    case "$board" in
        rpi4)
            BOARD_NAME="Raspberry Pi 4B"
            BOARD_ARCH="ARM64"
            export CROSS_COMPILE="${CROSS_COMPILE:-aarch64-elf-}"
            KERNEL_BUILD="$BUILD_OUT/kernel_arm64"
            KERNEL_TEMPLATE="arm64-rpi4"
            L4RE_BUILD="$BUILD_OUT/l4re_arm64"
            L4RE_TEMPLATE="arm64-rv-v8a"
            QEMU_CMD="qemu-system-aarch64"
            ;;
        bbb)
            BOARD_NAME="BeagleBone Black (AM335x)"
            BOARD_ARCH="ARM"
            export CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
            # macOS: brew 安装的工具链不在默认 PATH 中
            local _arm_brew_prefix
            _arm_brew_prefix="$(brew --prefix arm-unknown-linux-gnueabihf 2>/dev/null || true)"
            if [ -n "$_arm_brew_prefix" ] && [ -d "$_arm_brew_prefix/bin" ]; then
                export PATH="$_arm_brew_prefix/bin:$PATH"
            fi
            KERNEL_BUILD="$BUILD_OUT/kernel_arm"
            KERNEL_TEMPLATE="arm-omap3-am33xx"
            L4RE_BUILD="$BUILD_OUT/l4re_arm"
            L4RE_TEMPLATE="arm-omap3-am33xx"
            QEMU_CMD=""  # AM335x 无 QEMU 支持
            ;;
        virt)
            BOARD_NAME="QEMU ARM Virt (Cortex-A15)"
            BOARD_ARCH="ARM"
            export CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
            # macOS: brew 安装的工具链不在默认 PATH 中
            local _arm_brew_prefix
            _arm_brew_prefix="$(brew --prefix arm-unknown-linux-gnueabihf 2>/dev/null || true)"
            if [ -n "$_arm_brew_prefix" ] && [ -d "$_arm_brew_prefix/bin" ]; then
                export PATH="$_arm_brew_prefix/bin:$PATH"
            fi
            KERNEL_BUILD="$BUILD_OUT/kernel_virt"
            KERNEL_TEMPLATE="arm-virt-pl1"
            L4RE_BUILD="$BUILD_OUT/l4re_virt"
            L4RE_TEMPLATE="arm-virt-v7a"
            QEMU_CMD="qemu-system-arm -M virt -cpu cortex-a15"
            ;;
        *)
            error "未知目标板: $board
支持的目标板:
  rpi4   Raspberry Pi 4B (ARM64, 默认)
  bbb    BeagleBone Black (AM335x, ARM)
  virt   QEMU ARM Virt (Cortex-A15)"
            ;;
    esac
}

# ============================================================
# 检查构建依赖
# ============================================================
check_deps() {
    info "检查构建依赖..."

    # 检查交叉编译工具链
    if ! command -v "${CROSS_COMPILE}gcc" &>/dev/null; then
        case "$BOARD" in
            bbb)
                error "找不到交叉编译器: ${CROSS_COMPILE}gcc
请安装 ARM Linux 工具链:
  macOS:   brew tap messense/macos-cross-toolchains && brew install arm-unknown-linux-gnueabihf
  Ubuntu:  sudo apt install gcc-arm-linux-gnueabihf
  Fedora:  sudo dnf install gcc-arm-linux-gnu
或设置 CROSS_COMPILE 环境变量指向你的工具链前缀:
  export CROSS_COMPILE=arm-linux-gnueabihf-"
                ;;
            *)
                error "找不到交叉编译器: ${CROSS_COMPILE}gcc
请安装 aarch64 工具链:
  macOS:   brew install aarch64-elf-gcc  (或使用 aarch64-linux-gnu- 前缀的工具链)
  Ubuntu:  sudo apt install gcc-aarch64-linux-gnu
  Fedora:  sudo dnf install gcc-aarch64-linux-gnu
或设置 CROSS_COMPILE 环境变量指向你的工具链前缀:
  export CROSS_COMPILE=aarch64-none-elf-"
                ;;
        esac
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
    if [ -n "$QEMU_CMD" ] && ! command -v "$(echo "$QEMU_CMD" | cut -d' ' -f1)" &>/dev/null; then
        warn "找不到 $(echo "$QEMU_CMD" | cut -d' ' -f1)，构建可以继续但无法运行"
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

    if [ ! -f "$KERNEL_DIR/Makefile" ] || [ ! -f "$PROJ_ROOT/l4re/Makefile" ]; then
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
        mkdir -p "$(dirname "$KERNEL_BUILD")"
        info "创建内核构建目录 (模板: $KERNEL_TEMPLATE)..."
        $MAKE BUILDDIR="$KERNEL_BUILD" T="$KERNEL_TEMPLATE"
    fi

    # 编译内核
    info "编译内核..."
    $MAKE -C "$KERNEL_BUILD" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

    if [ -f "$KERNEL_BUILD/fiasco" ]; then
        info "内核已编译: $KERNEL_BUILD/fiasco"
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

    # 在 l4mk/pkg/ 下创建指向 l4re-core 和 pkg/ 下各包的符号链接
    local l4mk_pkg="$PROJ_ROOT/l4mk/pkg"
    if [ ! -e "$l4mk_pkg/l4re-core" ]; then
        ln -s ../../l4re "$l4mk_pkg/l4re-core"
        info "创建符号链接: l4mk/pkg/l4re-core -> ../../l4re"
    fi
    # 为 pkg/ 下每个包逐个创建符号链接
    for pkg_dir in "$PROJ_ROOT"/pkg/*/; do
        local pkg_name="$(basename "$pkg_dir")"
        if [ ! -e "$l4mk_pkg/$pkg_name" ]; then
            ln -s "../../pkg/$pkg_name" "$l4mk_pkg/$pkg_name"
            info "创建符号链接: l4mk/pkg/$pkg_name -> ../../pkg/$pkg_name"
        fi
    done

    # project.mk 的 find 命令不会跟随符号链接来发现 prj-config/aliases.d,
    # 需要将各子项目的别名文件合并到 l4mk/mk/aliases.d/ (该目录已在搜索路径中).
    # 多个包可能有同名的别名文件 (如 05-compiler-rt), 需要合并而非覆盖
    local aliases_dst="$PROJ_ROOT/l4mk/mk/aliases.d"
    local alias_dirs
    alias_dirs=$(find "$PROJ_ROOT/l4re" "$PROJ_ROOT/pkg" \
                 -maxdepth 5 -type d -name aliases.d \
                 -path "*/prj-config/aliases.d" 2>/dev/null || true)
    local aliases_changed=0
    for adir in $alias_dirs; do
        for f in "$adir"/*; do
            [ -f "$f" ] || continue
            local fname="$(basename "$f")"
            local dst="$aliases_dst/$fname"
            if [ ! -e "$dst" ]; then
                cp "$f" "$dst"
                aliases_changed=1
            else
                # 检查是否有尚未包含的别名定义行 (非注释、非空行)
                local need_merge=0
                while IFS= read -r line; do
                    [ -z "$line" ] && continue
                    case "$line" in \#*) continue ;; esac
                    if ! grep -qF "$line" "$dst" 2>/dev/null; then
                        need_merge=1
                        break
                    fi
                done < "$f"
                if [ "$need_merge" -eq 1 ]; then
                    echo "" >> "$dst"
                    cat "$f" >> "$dst"
                    aliases_changed=1
                fi
            fi
        done
    done
    if [ "$aliases_changed" -eq 1 ]; then
        info "已合并包别名定义到 l4mk/mk/aliases.d/"
    fi

    # 生成 Makeconf.local: 修复符号链接导致的路径问题
    # make -C 会解析符号链接, 导致 CURDIR 指向物理路径 (l4re/、pkg/drivers/),
    # 而非逻辑路径 (l4mk/pkg/l4re-core/、l4mk/pkg/drivers/).
    # 这使得包被误判为 "external package", 构建产物路径错误.
    # 此 Makeconf.local 将物理路径重映射回 l4mk/pkg/ 下的逻辑路径.
    local makeconf_local="$l4mk_dir/Makeconf.local"
    if [ ! -e "$makeconf_local" ]; then
        cat > "$makeconf_local" << 'MAKECONF_EOF'
# Auto-generated by build.sh — DO NOT EDIT
# Remap symlinked package paths back to l4mk/pkg/ logical paths
#
# Problem: make -C resolves symlinks, so packages accessed via
#   l4mk/pkg/l4re-core -> ../../l4re
#   l4mk/pkg/drivers   -> ../../pkg/drivers
# end up with CURDIR pointing to the physical path (l4re/*, pkg/drivers/*).
# This causes the build system to treat them as "external packages".
# Fix: remap CURDIR-based variables back to l4mk/pkg/ logical paths.

# Export absolute L4DIR so sub-makes through symlinked paths get the correct
# value instead of computing it from broken relative paths.
L4DIR := $(abspath $(L4DIR))
export L4DIR
export CROSS_COMPILE

ifdef OBJ_BASE
_L4_ABS := $(abspath $(L4DIR))
_PRJ := $(abspath $(L4DIR)/..)

# Skip remapping if we're inside the build directory (OBJ_BASE)
ifeq ($(patsubst $(OBJ_BASE)/%,,$(CURDIR)),$(CURDIR))

# l4re/<pkg>/ → l4mk/pkg/l4re-core/<pkg>/
ifneq ($(patsubst $(_PRJ)/l4re/%,,$(CURDIR)),$(CURDIR))
  _REL := $(patsubst $(_PRJ)/l4re/%,%,$(CURDIR))
  SRC_DIR    := $(_L4_ABS)/pkg/l4re-core/$(_REL)
  PKGDIR_ABS := $(abspath $(SRC_DIR)/$(PKGDIR))
  OBJ_DIR    := $(OBJ_BASE)/pkg/l4re-core/$(_REL)
  PKGDIR_OBJ := $(abspath $(OBJ_DIR)/$(PKGDIR))
  L4DIR      := $(_L4_ABS)
endif

# pkg/<name>/... → l4mk/pkg/<name>/...
ifneq ($(patsubst $(_PRJ)/pkg/%,,$(CURDIR)),$(CURDIR))
  _REL := $(patsubst $(_PRJ)/pkg/%,%,$(CURDIR))
  SRC_DIR    := $(_L4_ABS)/pkg/$(_REL)
  PKGDIR_ABS := $(abspath $(SRC_DIR)/$(PKGDIR))
  OBJ_DIR    := $(OBJ_BASE)/pkg/$(_REL)
  PKGDIR_OBJ := $(abspath $(OBJ_DIR)/$(PKGDIR))
  L4DIR      := $(_L4_ABS)
endif

endif
endif
MAKECONF_EOF
        info "生成 l4mk/Makeconf.local (路径重映射)"
    fi

    # 检查构建目录是否存在且配置完整
    if [ ! -d "$L4RE_BUILD" ] || [ ! -f "$L4RE_BUILD/.config.all" ]; then
        if [ -d "$L4RE_BUILD" ] && [ ! -f "$L4RE_BUILD/.config.all" ]; then
            warn "构建目录存在但配置不完整，重新初始化..."
            rm -rf "$L4RE_BUILD"
        fi

        info "创建 L4Re 构建目录 (模板: $L4RE_TEMPLATE)..."
        # PLATFORM_TYPE 应该与模板名称对应，从 defconfig 文件中提取
        # 但由于 defconfig 已包含正确的 PLATFORM_TYPE，我们传递空值以避免重复
        $MAKE -C "$l4mk_dir" B="$L4RE_BUILD" T="$L4RE_TEMPLATE" PLATFORM_TYPE= CROSS_COMPILE="${CROSS_COMPILE}"

        # 验证配置文件是否正确生成
        if [ ! -f "$L4RE_BUILD/.config.all" ]; then
            warn "配置文件未自动生成，手动运行 olddefconfig..."
            $MAKE -C "$l4mk_dir" O="$L4RE_BUILD" CROSS_COMPILE="${CROSS_COMPILE}" olddefconfig
        fi

        # 再次验证
        if [ ! -f "$L4RE_BUILD/.config.all" ]; then
            error "L4Re 构建目录配置失败，请检查构建环境"
        fi
        info "L4Re 构建目录已初始化"
    fi

    cd "$L4RE_BUILD"
    # 传递绝对 L4DIR 以避免符号链接解析导致的相对路径错误:
    # 包的 Makefile 使用 L4DIR ?= $(PKGDIR)/../../.. 来定位构建系统,
    # 但 make -C 会解析符号链接, 导致相对路径失效
    # 同时传递 CROSS_COMPILE 确保编译器一致性
    $MAKE L4DIR="$l4mk_dir" CROSS_COMPILE="${CROSS_COMPILE}" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
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

    if [ ! -f "$KERNEL_BUILD/fiasco" ]; then
        error "内核尚未编译，请先构建内核"
    fi

    # 设置模块搜索路径: 内核构建目录 + 配置文件
    export MODULE_SEARCH_PATH="$KERNEL_BUILD:$CONF_DIR"

    # 选择启动入口 (可通过环境变量 ENTRY 覆盖)
    # 可用入口: fiasco-base-test, shell, demo
    local entry="${ENTRY:-native-shell}"
    info "使用启动入口: $entry"

    cd "$L4RE_BUILD"
    $MAKE E="$entry" elfimage

    # 复制引导镜像到输出目录
    if [ -f "$L4RE_BUILD/images/bootstrap.elf" ]; then
        cp "$L4RE_BUILD/images/bootstrap.elf" "$BUILD_OUT/"
        info "引导镜像已生成: $BUILD_OUT/bootstrap.elf"
        if [ "$BOARD" = "virt" ]; then
            info "可使用 ./run_qemu_virt.sh 启动系统"
        else
            info "真机部署需要通过 U-Boot 从 SD 卡加载"
        fi
    else
        error "引导镜像生成失败"
    fi
}

# ============================================================
# 清理构建产物
# ============================================================
do_clean() {
    local clean_type="${1:-full}"

    if [ "$clean_type" = "full" ]; then
        info "完全清理构建产物..."

        # 清理内核构建目录
        if [ -d "$KERNEL_BUILD" ]; then
            info "  清理内核构建目录: $KERNEL_BUILD"
            rm -rf "$KERNEL_BUILD"
        fi

        # 清理 L4Re 构建目录
        if [ -d "$L4RE_BUILD" ]; then
            info "  清理 L4Re 构建目录: $L4RE_BUILD"
            rm -rf "$L4RE_BUILD"
        fi

        # 清理 artifacts 目录
        if [ -d "$BUILD_OUT/artifacts" ]; then
            info "  清理 artifacts 目录"
            rm -rf "$BUILD_OUT/artifacts"
        fi

        # 清理 bootstrap.elf
        if [ -f "$BUILD_OUT/bootstrap.elf" ]; then
            info "  清理 bootstrap.elf"
            rm -f "$BUILD_OUT/bootstrap.elf"
        fi

    else
        # 轻度清理：只清理编译中间文件
        info "轻度清理（保留构建目录结构）..."

        # 清理内核
        if [ -d "$KERNEL_BUILD" ]; then
            info "  清理内核中间文件"
            $MAKE -C "$KERNEL_BUILD" clean 2>/dev/null || true
        fi

        # 清理 L4Re
        if [ -d "$L4RE_BUILD" ]; then
            info "  清理 L4Re 中间文件"
            $MAKE -C "$L4RE_BUILD" clean 2>/dev/null || true
        fi

        # 清理 bootstrap.elf
        if [ -f "$BUILD_OUT/bootstrap.elf" ]; then
            rm -f "$BUILD_OUT/bootstrap.elf"
        fi
    fi

    info "清理完成"
}

# ============================================================
# 收集构建产物
# ============================================================
collect_artifacts() {
    info "收集构建产物到 $BUILD_OUT/artifacts/..."

    local artifacts_dir="$BUILD_OUT/artifacts"
    mkdir -p "$artifacts_dir"

    # 收集内核
    if [ -f "$KERNEL_BUILD/fiasco" ]; then
        cp "$KERNEL_BUILD/fiasco" "$artifacts_dir/fiasco-${BOARD}"
        info "✓ 内核: fiasco-${BOARD}"
    fi

    # 收集 bootstrap（完整引导镜像）
    if [ -f "$L4RE_BUILD/images/bootstrap.elf" ]; then
        cp "$L4RE_BUILD/images/bootstrap.elf" "$artifacts_dir/bootstrap-image-${BOARD}.elf"
        info "✓ Bootstrap 完整镜像: bootstrap-image-${BOARD}.elf"
    else
        warn "✗ Bootstrap 完整镜像未找到 (需要运行: ./build.sh --board ${BOARD} bootstrap)"
    fi

    # 收集 build 根目录下的 bootstrap.elf（如果存在）
    if [ -f "$BUILD_OUT/bootstrap.elf" ]; then
        cp "$BUILD_OUT/bootstrap.elf" "$artifacts_dir/bootstrap-final-${BOARD}.elf"
        info "✓ 最终引导镜像: bootstrap-final-${BOARD}.elf"
    fi

    # 创建说明文件
    cat > "$artifacts_dir/README.md" << EOF
# TuringOS 构建产物

构建时间: $(date)
目标板: $BOARD_NAME ($BOARD_ARCH)
交叉编译器: ${CROSS_COMPILE}gcc

## 文件列表

### 内核
EOF

    if [ -f "$artifacts_dir/fiasco-${BOARD}" ]; then
        echo "- \`fiasco-${BOARD}\` - Fiasco 微内核" >> "$artifacts_dir/README.md"
    fi

    cat >> "$artifacts_dir/README.md" << EOF

### Bootstrap
EOF

    if [ -f "$artifacts_dir/bootstrap-image-${BOARD}.elf" ]; then
        echo "- \`bootstrap-image-${BOARD}.elf\` - 包含所有模块的完整引导镜像" >> "$artifacts_dir/README.md"
    fi
    if [ -f "$artifacts_dir/bootstrap-final-${BOARD}.elf" ]; then
        echo "- \`bootstrap-final-${BOARD}.elf\` - 最终引导镜像（包含内核和模块）" >> "$artifacts_dir/README.md"
    fi

    cat >> "$artifacts_dir/README.md" << EOF

## 生成完整镜像

如果需要包含所有模块的完整引导镜像，请运行：
\`\`\`bash
./build.sh --board ${BOARD} bootstrap
\`\`\`

---
自动生成于 $(date +"%Y-%m-%d %H:%M:%S")
EOF

    info "构建产物已收集到: $artifacts_dir"
    ls -lh "$artifacts_dir"
}

# ============================================================
# 主流程
# ============================================================
main() {
    # 解析 --board 参数
    local board="rpi4"
    local target=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --board)
                board="$2"
                shift 2
                ;;
            -h|--help)
                show_usage
                exit 0
                ;;
            *)
                target="$1"
                shift
                ;;
        esac
    done

    target="${target:-all}"

    # 设置目标板相关变量
    BOARD="$board"
    setup_board "$board"

    echo "========================================"
    echo "  TuringOS 构建系统"
    echo "  目标板: $BOARD_NAME ($BOARD_ARCH)"
    echo "  交叉编译器: ${CROSS_COMPILE}gcc"
    echo "========================================"

    case "$target" in
        all)
            check_deps
            init_submodules
            build_kernel
            build_l4re
            build_bootstrap
            collect_artifacts
            ;;
        kernel)
            check_deps
            init_submodules
            build_kernel
            collect_artifacts
            ;;
        l4re)
            check_deps
            build_l4re
            collect_artifacts
            ;;
        bootstrap)
            build_bootstrap
            collect_artifacts
            ;;
        collect)
            collect_artifacts
            ;;
        clean)
            do_clean "light"
            ;;
        cleanall)
            do_clean "full"
            ;;
        *)
            show_usage
            exit 1
            ;;
    esac

    info "构建完成!"
}

show_usage() {
    echo "用法: $0 [--board <board>] {all|kernel|l4re|bootstrap|collect|clean|cleanall}"
    echo ""
    echo "目标板 (--board):"
    echo "  rpi4        Raspberry Pi 4B, ARM64 (默认)"
    echo "  bbb         BeagleBone Black, AM335x ARM"
    echo "  virt        QEMU ARM Virt (Cortex-A15)"
    echo ""
    echo "构建目标:"
    echo "  all         完整构建 (内核 + L4Re + 引导镜像 + 收集产物)"
    echo "  kernel      仅构建 Fiasco 内核"
    echo "  l4re        仅构建 L4Re 运行时"
    echo "  bootstrap   仅生成引导镜像 (需先完成 kernel 和 l4re)"
    echo "  collect     收集构建产物到 build/artifacts 目录"
    echo "  clean       清理编译中间文件（保留构建目录结构）"
    echo "  cleanall    完全清理构建目录（删除 build/kernel_* 和 build/l4re_*）"
    echo ""
    echo "示例:"
    echo "  $0                       # 默认 RPi4 全量构建"
    echo "  $0 --board bbb l4re      # 为 BBB 构建 L4Re"
    echo "  $0 --board bbb all       # BBB 全量构建"
    echo "  $0 --board bbb collect   # 仅收集 BBB 构建产物"
    echo ""
    echo "环境变量:"
    echo "  CROSS_COMPILE  交叉编译器前缀 (会根据 --board 自动设置)"
    echo ""
    echo "构建产物将收集到: build/artifacts/"
}

main "$@"
