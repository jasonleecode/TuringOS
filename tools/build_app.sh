#!/bin/bash
# 快速构建单个 app，不触发 L4Re 完整构建
#
# 用法: ./build_app.sh <app_name> [board]
#   例如: ./build_app.sh native_shell virt

set -e

APP_NAME="${1}"
BOARD="${2:-virt}"

if [ -z "$APP_NAME" ]; then
    echo "用法: $0 <app_name> [board]"
    echo "示例: $0 native_shell virt"
    exit 1
fi

# 配置
BUILD_DIR="$PWD/build/l4re_${BOARD}"
BIN_DIR="$BUILD_DIR/bin/arm_armv7a/std/l4f"

# 查找 app 位置（支持 pkg/app 和 pkg/drivers）
if [ -d "$PWD/pkg/app/${APP_NAME}" ]; then
    APP_SRC="$PWD/pkg/app/${APP_NAME}"
    APP_PKG="pkg/app/${APP_NAME}"
elif [ -d "$PWD/pkg/drivers/${APP_NAME}" ]; then
    APP_SRC="$PWD/pkg/drivers/${APP_NAME}"
    APP_PKG="pkg/drivers/${APP_NAME}"
else
    echo "错误: App 不存在: ${APP_NAME}"
    echo "查找路径: pkg/app/${APP_NAME} 或 pkg/drivers/${APP_NAME}"
    exit 1
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "错误: 请先构建 L4Re:"
    echo "  ./build.sh --board $BOARD l4re"
    exit 1
fi

# 颜色输出
info() { echo -e "\033[0;32m>>> $1\033[0m"; }
warn() { echo -e "\033[1;33m!!! $1\033[0m"; }
error() { echo -e "\033[0;31m[ERROR] $1\033[0m"; exit 1; }

info "快速构建 App: $APP_NAME (板型: $BOARD)"

# Toolchain
export CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
if [[ "$OSTYPE" == "darwin"* ]]; then
    ARM_PREFIX=$(brew --prefix arm-unknown-linux-gnueabihf 2>/dev/null || echo "")
    if [ -n "$ARM_PREFIX" ] && [ -d "$ARM_PREFIX/toolchain/bin" ]; then
        export PATH="$ARM_PREFIX/toolchain/bin:$PATH"
    fi
fi

if ! command -v ${CROSS_COMPILE}gcc &> /dev/null; then
    error "${CROSS_COMPILE}gcc 不存在"
fi

# 确保 CC/CXX 环境变量被设置
export CC="${CROSS_COMPILE}gcc"
export CXX="${CROSS_COMPILE}g++"

# 使用 L4Re 构建系统，但只编译指定的 app
cd "$BUILD_DIR"

info "编译 $APP_NAME (${APP_PKG})..."
gmake \
    L4DIR="$PWD/../../l4mk" \
    CROSS_COMPILE="${CROSS_COMPILE}" \
    CC="${CC}" \
    CXX="${CXX}" \
    -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) \
    ${APP_PKG} \
    2>&1 | grep -E "Compiling|Linking|built|Error|error" || true

# 检查结果
if [ -f "$BIN_DIR/${APP_NAME}" ]; then
    info "✓ 构建成功: $BIN_DIR/${APP_NAME}"
    ls -lh "$BIN_DIR/${APP_NAME}"
else
    error "构建失败"
fi

info "提示: 使用 './build.sh --board $BOARD bootstrap' 重新生成启动镜像"
