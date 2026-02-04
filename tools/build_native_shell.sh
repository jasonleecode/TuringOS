#!/bin/bash
# Standalone build script for native_shell
# Uses already-built L4Re libraries from build/l4re_virt

set -e

# Configuration
BOARD=${1:-virt}
BUILD_DIR="$PWD/build/l4re_${BOARD}"
SRC_DIR="$PWD/pkg/app/native_shell/server/src"
OUT_DIR="$BUILD_DIR/bin/arm_armv7a/std/l4f"
OBJ_DIR="$BUILD_DIR/obj/native_shell"

# Toolchain
export CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
CC="${CROSS_COMPILE}gcc"
CXX="${CROSS_COMPILE}g++"

# ARM toolchain setup for macOS Homebrew
if [[ "$OSTYPE" == "darwin"* ]]; then
    ARM_PREFIX=$(brew --prefix arm-unknown-linux-gnueabihf 2>/dev/null || echo "")
    if [ -n "$ARM_PREFIX" ] && [ -d "$ARM_PREFIX/toolchain/bin" ]; then
        export PATH="$ARM_PREFIX/toolchain/bin:$PATH"
    fi
fi

# Check toolchain
if ! command -v ${CC} &> /dev/null; then
    echo "Error: ${CC} not found"
    exit 1
fi

echo "========================================="
echo "  Building native_shell"
echo "  Board: $BOARD"
echo "  Compiler: $CC"
echo "========================================="

# Create output directories
mkdir -p "$OBJ_DIR"
mkdir -p "$OUT_DIR"

# L4Re paths
L4_LIB="$BUILD_DIR/lib/arm_armv7a/std/l4f"
L4_INC="$BUILD_DIR/include"

# Check if L4Re is built
if [ ! -d "$L4_LIB" ]; then
    echo "Error: L4Re not built. Run: ./build.sh --board $BOARD l4re"
    exit 1
fi

# Compiler flags (simplified from L4Re's prog.mk)
CFLAGS="-march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard"
CFLAGS="$CFLAGS -I$L4_INC"
CFLAGS="$CFLAGS -I$L4_INC/contrib/readline"
CFLAGS="$CFLAGS -I$L4_INC/l4/sys"
CFLAGS="$CFLAGS -O2 -g -Wall"

# Linker flags
LDFLAGS="-L$L4_LIB"
LDFLAGS="$LDFLAGS -lreadline -lstdc++ -lc"
LDFLAGS="$LDFLAGS -static"

echo ">>> Compiling main.c..."
$CC $CFLAGS -c "$SRC_DIR/main.c" -o "$OBJ_DIR/main.o"

echo ">>> Compiling commands.c..."
$CC $CFLAGS -c "$SRC_DIR/commands.c" -o "$OBJ_DIR/commands.o"

echo ">>> Linking native_shell..."
$CC $CFLAGS "$OBJ_DIR/main.o" "$OBJ_DIR/commands.o" $LDFLAGS -o "$OUT_DIR/native_shell"

# Strip for smaller binary
${CROSS_COMPILE}strip "$OUT_DIR/native_shell"

echo ">>> Build complete: $OUT_DIR/native_shell"
ls -lh "$OUT_DIR/native_shell"
