#!/usr/bin/env bash
# Inspect a published board image or run it with its matching QEMU machine.
set -eu
root="$(cd "$(dirname "$0")" && pwd)"
board=rpi4
entry=
seconds=10
check=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --board) board=${2:?missing board}; shift 2 ;;
        --timeout) seconds=${2:?missing timeout}; shift 2 ;;
        --check) check=1; shift ;;
        --*) echo "Unknown argument: $1" >&2; exit 2 ;;
        *) entry=$1; shift ;;
    esac
done
case "$board" in rpi4|bbb|virt|imx6ul) ;; *) echo "Unknown board: $board" >&2; exit 2;; esac
source "$root/conf/boards/$board.conf"
out=${BUILD_OUT:-$root/build}
if [[ -z $entry ]]; then
    entry=$SHELL_ENTRY
    if grep -qs '^# CONFIG_PKG_NATIVE_SHELL is not set$' "$out/l4re_$BUILD_KEY/.kconfig"; then entry=minimal; fi
fi
image="$out/artifacts/$board/$entry/bootstrap.elf"
if [[ ! -f $image ]]; then image="$out/l4re_$BUILD_KEY/images/bootstrap_${entry}.elf"; fi
[[ -s $image ]] || { echo "请先运行 ENTRY=$entry ./build.sh --board $board all" >&2; exit 1; }
echo "$BOARD_NAME — $image"
readelf -h "$image"
if (( check )); then exit 0; fi
case "$board" in
    bbb) echo "QEMU 未实现 AM335x，请在 BBB 上运行此镜像。" >&2; exit 2 ;;
    rpi4) run=(qemu-system-aarch64 -M raspi4b -serial null -serial stdio -display none)
          if ! qemu-system-aarch64 -machine help | grep '^raspi4b ' >/dev/null; then
              echo "当前 QEMU 缺少 raspi4b 机型。" >&2; exit 2
          fi ;;
    virt) exec "$root/run_qemu_virt.sh" --cfg "$entry" ;;
    imx6ul) exec "$root/run_qemu_imx6ul.sh" --cfg "$entry" --timeout "$seconds" ;;
esac
if [[ $seconds == 0 ]]; then exec "${run[@]}" -kernel "$image"; fi
status=0
timeout "$seconds" "${run[@]}" -kernel "$image" || status=$?
[[ $status == 124 ]] || exit "$status"
