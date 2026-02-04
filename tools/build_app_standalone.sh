#!/bin/bash
# 完全独立构建 app，不依赖 L4Re 构建系统
# 适合快速迭代开发

set -e

APP_NAME="${1}"
BOARD="${2:-virt}"

if [ -z "$APP_NAME" ]; then
    echo "用法: $0 <app_name> [board]"
    exit 1
fi

# 路径配置
BUILD_DIR="$PWD/build/l4re_${BOARD}"
APP_SRC="$PWD/pkg/app/${APP_NAME}/server/src"
OBJ_DIR="$BUILD_DIR/obj/app/${APP_NAME}"
BIN_DIR="$BUILD_DIR/bin/arm_armv7a/std/l4f"

# L4Re SDK 路径
L4_LIB="$BUILD_DIR/lib/arm_armv7a/std/l4f"
L4_INC="$BUILD_DIR/include"

# 检查
[ -d "$APP_SRC" ] || { echo "App 源码不存在: $APP_SRC"; exit 1; }
[ -d "$L4_LIB" ] || { echo "请先构建 L4Re: ./build.sh --board $BOARD l4re"; exit 1; }

# Toolchain
export CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
CC="${CROSS_COMPILE}gcc"
CXX="${CROSS_COMPILE}g++"

if [[ "$OSTYPE" == "darwin"* ]]; then
    ARM_PREFIX=$(brew --prefix arm-unknown-linux-gnueabihf 2>/dev/null || echo "")
    if [ -n "$ARM_PREFIX" ] && [ -d "$ARM_PREFIX/toolchain/bin" ]; then
        export PATH="$ARM_PREFIX/toolchain/bin:$PATH"
    fi
fi

# 输出
info() { echo -e "\033[0;32m>>> $1\033[0m"; }

info "独立构建 $APP_NAME"

# 创建目录
mkdir -p "$OBJ_DIR" "$BIN_DIR"

# 编译标志
CFLAGS="-march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard"
CFLAGS="$CFLAGS -O2 -g -Wall"
CFLAGS="$CFLAGS -I$L4_INC"
CFLAGS="$CFLAGS -I$L4_INC/l4/sys"

# 链接标志
LDFLAGS="-L$L4_LIB"
LDFLAGS="$LDFLAGS -static"

# 查找源文件
SRC_FILES=$(find "$APP_SRC" -name "*.c" -o -name "*.cc")

# 编译
for src in $SRC_FILES; do
    obj="$OBJ_DIR/$(basename ${src%.*}).o"
    info "编译 $(basename $src)..."

    if [[ "$src" == *.cc ]]; then
        $CXX $CFLAGS -c "$src" -o "$obj"
    else
        $CC $CFLAGS -c "$src" -o "$obj"
    fi
done

# 链接
info "链接 $APP_NAME..."
OBJ_FILES=$(find "$OBJ_DIR" -name "*.o")

# 根据 app 需要的库来链接
case "$APP_NAME" in
    native_shell)
        LIBS="-lreadline -lstdc++ -lc -lsupc++ -ll4re -ll4sys"
        ;;
    *)
        LIBS="-lstdc++ -lc -ll4re -ll4sys"
        ;;
esac

$CXX $CFLAGS $OBJ_FILES $LDFLAGS $LIBS -o "$BIN_DIR/$APP_NAME"

${CROSS_COMPILE}strip "$BIN_DIR/$APP_NAME"

info "✓ 完成: $BIN_DIR/$APP_NAME"
ls -lh "$BIN_DIR/$APP_NAME"
info "重新生成镜像: ./build.sh --board $BOARD bootstrap"
