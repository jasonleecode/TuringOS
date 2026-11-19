#!/usr/bin/env bash
# Cold build, incremental link and publication regression checks.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
export BUILD_OUT
BUILD_OUT=$(mktemp -d "${TMPDIR:-/tmp}/turingos-build.XXXXXX")
trap 'rm -rf "$BUILD_OUT"' EXIT
unset ENTRY
build="$root/build.sh"
SECONDS=0
"$build" --board virt all
echo "virt cold build: ${SECONDS}s"
virt="$BUILD_OUT/l4re_virt"
cmp "$virt/images/bootstrap_native-shell.elf" "$BUILD_OUT/artifacts/virt/native-shell/bootstrap.elf"

# Only change the archive timestamp in this test's private object tree.
demo=$(find "$virt/pkg/lvgl/demo" -name lvgl-demo -type f -print -quit)
archive=$(find "$virt/lib" -path '*/std/l4f/libklog.a' -print -quit)
test -s "$demo"
test -s "$archive"
kernel="$BUILD_OUT/kernel_virt/fiasco"
checkpoint="$BUILD_OUT/no-change-start"
touch "$checkpoint"
SECONDS=0
"$build" --board virt all | tee "$BUILD_OUT/no-change.log"
if grep -E ' (Compiling|Linking) ' "$BUILD_OUT/no-change.log"; then
    echo "An unchanged build compiled or linked a target" >&2
    exit 1
fi
test ! "$kernel" -nt "$checkpoint"
test ! "$demo" -nt "$checkpoint"
echo "virt no-change build: ${SECONDS}s"

touch "$archive"
SECONDS=0
ENTRY=lvgl-demo "$build" --board virt --pkg klog
test "$demo" -nt "$archive"
echo "virt static library relink and image: ${SECONDS}s"
ENTRY=wamr "$build" --board virt --pkg wamr
test -s "$BUILD_OUT/artifacts/virt/wamr/bootstrap.elf"

"$build" --board imx6ul all
imx="$BUILD_OUT/artifacts/imx6ul/imx6ul-native-shell/bootstrap.elf"
cmp "$imx" "$BUILD_OUT/l4re_imx6ul/images/bootstrap_imx6ul-native-shell.elf"
"$build" --board virt cleanall
test ! -d "$BUILD_OUT/artifacts/virt"
test -s "$imx"

make -C "$root" BOARD=virt allnoconfig
"$build" --board virt all
test -s "$BUILD_OUT/artifacts/virt/minimal/bootstrap.elf"
test -z "$(find "$virt/bin" -name native_shell -print -quit)"
echo 'PASS: cold images, static relink, package dependencies, board isolation and minimal configuration'
