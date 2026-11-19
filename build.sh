#!/usr/bin/env bash
set -euo pipefail

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd -P)"
CONF_DIR="$PROJ_ROOT/conf"
KERNEL_DIR="$PROJ_ROOT/kernel"
BUILD_OUT="${BUILD_OUT:-$PROJ_ROOT/build}"
MAKE=$(command -v gmake || command -v make)
JOBS=${JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
info() { echo ">>> $*"; }
warn() { echo ">>> $*" >&2; }
error() { warn "$@"; exit 1; }

usage() {
    cat <<'HELP'
./build.sh [--board rpi4|bbb|virt|imx6ul] [all|kernel|l4re|bootstrap|collect|clean|cleanall]
./build.sh --board virt --pkg NAME [--force] [--compile-only]
./build.sh --board virt menuconfig
ENTRY selects a boot entry. BUILD_OUT selects the output root. JOBS controls parallelism.
Each board publishes artifacts/<board>/<entry>/bootstrap.elf.
HELP
}

requested_entry=${ENTRY:-}
unset ENTRY
BOARD=rpi4
target=all
pkg=
force=0
compile_only=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --board) BOARD=${2:?missing board}; shift 2 ;;
        --pkg) pkg=${2:?missing package}; target=pkg; shift 2 ;;
        --compile-only) compile_only=1; shift ;;
        --force) force=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) target=$1; shift ;;
    esac
done
case "$BOARD" in virt|imx6ul|bbb|rpi4) ;; *) error "Unknown board: $BOARD";; esac
[[ $compile_only == 0 || $target == pkg ]] || error "--compile-only requires --pkg NAME"
source "$CONF_DIR/boards/$BOARD.conf"
mkdir -p "$BUILD_OUT"
BUILD_OUT=$(cd "$BUILD_OUT" && pwd -P)
KERNEL_BUILD="$BUILD_OUT/kernel_$BUILD_KEY"
L4RE_BUILD="$BUILD_OUT/l4re_$BUILD_KEY"
ARTIFACTS="$BUILD_OUT/artifacts/$BOARD"
# The shell retains the open file description while make, packaging or clean runs.
exec 9>>"$BUILD_OUT/.$BOARD.lock"
python3 -c 'import fcntl; fcntl.flock(9, fcntl.LOCK_EX)'
JOBS=${JOBS:?}
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || error "Invalid JOBS: $JOBS"
parallel=(-j"$JOBS")
if [[ ${TURINGOS_JOBSERVER:-0} == 1 ]]; then parallel=(); fi
if [[ $ARCH == arm ]]; then
    toolbin=/opt/arm-gnu-toolchain-12.3.rel1-x86_64-arm-none-linux-gnueabihf/bin
    if [[ -d $toolbin ]]; then
        export PATH="$toolbin:$PATH"
        : "${CROSS_COMPILE:=arm-none-linux-gnueabihf-}"
    else
        : "${CROSS_COMPILE:=arm-linux-gnueabihf-}"
    fi
elif command -v aarch64-elf-gcc >/dev/null; then
    : "${CROSS_COMPILE:=aarch64-elf-}"
else
    : "${CROSS_COMPILE:=aarch64-linux-gnu-}"
fi
export CROSS_COMPILE
init_sources() {
    if [[ ! -f $KERNEL_DIR/Makefile || ! -f $PROJ_ROOT/l4mk/Makefile ||
          ! -f $PROJ_ROOT/l4re/Makefile || ! -f $PROJ_ROOT/pkg/filesystem/ext4/contrib/include/ext4.h ||
          ! -f $PROJ_ROOT/pkg/lvgl/liblvgl/src/lvgl/lvgl.h ]]; then
        git -C "$PROJ_ROOT" submodule update --init --recursive
    fi
}
build_kernel() (
    info "构建 Fiasco 内核 (模板: $KERNEL_TEMPLATE)..."
    cd "$KERNEL_DIR"

    # 检查 build 目录的 Makefile 中 srcdir 是否正确
    local need_rebuild=0 previous=""
    if [ -f "$KERNEL_BUILD/Makefile" ]; then
        if ! grep -qF "srcdir  := $KERNEL_DIR/src" "$KERNEL_BUILD/Makefile"; then
            warn "内核构建目录的源码路径过期，将重新创建"
            need_rebuild=1
        fi
    else
        need_rebuild=1
    fi

    if [ "$need_rebuild" -eq 1 ]; then
        if [[ -d $KERNEL_BUILD ]]; then
            previous=$(mktemp -d "$BUILD_OUT/.previous-kernel-$BUILD_KEY.XXXXXX")
            mv "$KERNEL_BUILD" "$previous/"
            info "Previous kernel build: $previous"
        fi
        mkdir -p "$(dirname "$KERNEL_BUILD")"
        info "创建内核构建目录 (模板: $KERNEL_TEMPLATE)..."
        $MAKE BUILDDIR="$KERNEL_BUILD" T="$KERNEL_TEMPLATE"
        if [[ -n $previous && -s $previous/${KERNEL_BUILD##*/}/globalconfig.out ]]; then
            cp "$previous/${KERNEL_BUILD##*/}/globalconfig.out" "$KERNEL_BUILD/globalconfig.out"
            $MAKE -C "$KERNEL_BUILD" olddefconfig
        fi
    fi

    # imx6ul: 禁用 HYP/虚拟化 — mcimx6ul-evk QEMU 不支持 PSCI HYP 模式切换
    if [[ $BOARD == imx6ul ]] && grep -qx CONFIG_CPU_VIRT=y "$KERNEL_BUILD/globalconfig.out"; then
        info "imx6ul: 禁用 CONFIG_CPU_VIRT (mcimx6ul-evk 不支持 HYP 切换)"
        sed -i 's/^CONFIG_CPU_VIRT=y/# CONFIG_CPU_VIRT is not set/' "$KERNEL_BUILD/globalconfig.out"
        $MAKE -C "$KERNEL_BUILD" olddefconfig CROSS_COMPILE="$CROSS_COMPILE"
    fi

    # A new invocation alone must not invalidate the kernel link.
    : "${SOURCE_DATE_EPOCH:=$(git -C "$KERNEL_DIR" log -1 --format=%ct 2>/dev/null || echo 0)}"
    [[ $SOURCE_DATE_EPOCH =~ ^[0-9]+$ && ${KBUILD_BUILD_VERSION:-1} =~ ^[0-9]+$ ]] || error "Invalid kernel build epoch or version"
    export SOURCE_DATE_EPOCH

    # 编译内核
    info "编译内核..."
    $MAKE -C "$KERNEL_BUILD" "${parallel[@]}"

    if [ -f "$KERNEL_BUILD/fiasco" ]; then
        info "内核已编译: $KERNEL_BUILD/fiasco"
    else
        error "内核编译失败: 找不到 fiasco 二进制文件"
    fi
)

board_make() {
    "$MAKE" -C "$L4RE_BUILD" L4DIR="$PROJ_ROOT/l4mk" CROSS_COMPILE="$CROSS_COMPILE" "$@"
}

# Keep the board and architecture even when resetting optional product packages.
board_defaults() {
    if [[ $BOARD == rpi4 ]]; then
        echo CONFIG_BUILD_ARCH_arm64=y
        echo CONFIG_PLATFORM_TYPE_rpi=y
        cat "$PROJ_ROOT/defconfig"
    else
        cat "$PROJ_ROOT/l4mk/mk/defconfig/config.$L4RE_TEMPLATE"
    fi
}

prepare_board() {
    init_sources
    python3 "$PROJ_ROOT/tools/build_packages.py" register "$PROJ_ROOT"
    local saved_config=""
    if [[ -f $L4RE_BUILD/Makefile ]] &&
       { ! grep -Fxq "OBJ := $L4RE_BUILD" "$L4RE_BUILD/Makefile" ||
         ! grep -Fxq "SRC := $PROJ_ROOT/l4mk" "$L4RE_BUILD/Makefile"; }; then
        # Generated makefiles, command records and install links embed absolute paths.
        # Preserve the old tree (including local overrides) and re-use its Kconfig choices.
        local previous
        previous=$(mktemp -d "$BUILD_OUT/.previous-$BUILD_KEY.XXXXXX")
        mv "$L4RE_BUILD" "$previous/"
        saved_config="$previous/${L4RE_BUILD##*/}/.kconfig"
        info "Recreating relocated board tree; previous outputs: $previous"
    fi
    if [[ -f $L4RE_BUILD/.kconfig && ! -f $L4RE_BUILD/.config.all ]]; then
        "$MAKE" -C "$PROJ_ROOT/l4mk" O="$L4RE_BUILD" olddefconfig
    fi
    if [[ ! -f $L4RE_BUILD/.config.all ]]; then
        [[ ! -e $L4RE_BUILD ]] || error "Incomplete board tree: $L4RE_BUILD (use cleanall to reset)"
        local defaults
        defaults=$(mktemp "$BUILD_OUT/.defconfig.$BOARD.XXXXXX")
        if [[ -s $saved_config ]]; then
            sed '/^CONFIG_PLATFORM_TYPE_.*=y$/d' "$saved_config" > "$defaults"
        else
            board_defaults | sed '/^CONFIG_PLATFORM_TYPE_.*=y$/d' > "$defaults"
        fi
        "$MAKE" -C "$PROJ_ROOT/l4mk" B="$L4RE_BUILD" DEFCONFIG="$defaults" PLATFORM_TYPE="$PLATFORM"
        rm -f "$defaults"
    fi
    mkdir -p "$L4RE_BUILD/conf"
    local board_mk="$L4RE_BUILD/conf/Makeconf.local"
    if [[ ! -e $board_mk ]]; then
        ln -s "$CONF_DIR/Makeconf.board" "$board_mk"
    elif [[ ! $board_mk -ef $CONF_DIR/Makeconf.board ]] &&
         ! grep -qF "$CONF_DIR/Makeconf.board" "$board_mk"; then
        printf '\ninclude %s\n' "$CONF_DIR/Makeconf.board" >> "$board_mk"
    fi
    # Older trees predate the product Kconfig or were configured for VExpress.
    if ! grep -qx "CONFIG_PLATFORM_TYPE=$PLATFORM" "$L4RE_BUILD/.config.all"; then
        warn "Updating L4Re platform to $PLATFORM for $BOARD"
        sed '/^CONFIG_PLATFORM_TYPE_.*=y$/d' "$L4RE_BUILD/.kconfig" > "$L4RE_BUILD/.kconfig.board"
        echo "CONFIG_PLATFORM_TYPE_$PLATFORM=y" >> "$L4RE_BUILD/.kconfig.board"
        mv "$L4RE_BUILD/.kconfig.board" "$L4RE_BUILD/.kconfig"
        board_make olddefconfig
    elif ! grep -q 'CONFIG_PKG_NATIVE_SHELL' "$L4RE_BUILD/.kconfig"; then
        board_make olddefconfig
    fi
}

configure() {
    prepare_board
    case "$target" in
        defconfig) board_defaults > "$L4RE_BUILD/.kconfig"; board_make olddefconfig ;;
        allnoconfig|allyesconfig)
            local sym new="$L4RE_BUILD/.kconfig.product"
            sed '/^\(# \)\?CONFIG_PKG_/d' "$L4RE_BUILD/.kconfig" > "$new"
            while read -r sym; do
                if [[ $target == allnoconfig ]]; then echo "# CONFIG_$sym is not set"
                else echo "CONFIG_$sym=y"; fi
            done < <(awk '$1=="config" && $2 ~ /^PKG_/ {print $2}' "$PROJ_ROOT/Kconfig") >> "$new"
            mv "$new" "$L4RE_BUILD/.kconfig"
            board_make olddefconfig ;;
        *) board_make "$target" ;;
    esac
}
select_entry() {
    if [[ -n $requested_entry ]]; then entry=$requested_entry
    elif grep -qx CONFIG_PKG_NATIVE_SHELL=y "$L4RE_BUILD/.config.all"; then entry=$SHELL_ENTRY
    else entry=minimal; fi
    [[ $entry =~ ^[[:alnum:]_-]+$ ]] || error "Invalid entry: $entry"
    image="$L4RE_BUILD/images/bootstrap_${entry}.elf"
}

resolve_package() {
    local name=$1 candidate
    [[ $name =~ ^[[:alnum:]_+.-]+(/[[:alnum:]_+.-]+)*$ && $name != *..* ]] || error "Invalid package: $name"
    if [[ -f $PROJ_ROOT/l4mk/pkg/$name/Control ]]; then echo "pkg/$name"; return; fi
    local matches=()
    for candidate in "$PROJ_ROOT/l4mk/pkg"/*/"$name"/Control; do
        [[ -f $candidate ]] || continue
        candidate=${candidate#"$PROJ_ROOT/l4mk/"}; matches+=("${candidate%/Control}")
    done
    [[ ${#matches[@]} == 1 ]] || error "Unknown or ambiguous package: $name (use its path under pkg/)"
    echo "${matches[0]}"
}

build_product() {
    local roots extra name t
    roots=$(awk -v entry="$entry" -v board="$BOARD" '
      /^entry(\[.*\])? / {active=($2==entry); if(active) found=1}
      active && /^# board / && $3!=board {invalid=1}
      active && /^# build-packages / {sub(/^# build-packages /,""); print}
      END {if (!found || invalid) exit 1}' "$CONF_DIR/modules.list") || error "Boot entry $entry is unavailable on $BOARD"
    local goals=(pkg/bootstrap pkg/l4re-core/sigma0 pkg/l4re-core/moe pkg/l4re-core/l4re_itas pkg/l4re-core/ned)
    for name in $roots; do goals+=("pkg/$name"); done
    # Package options add roots; L4Re resolves their transitive Control dependencies.
    for t in $(sed -n 's/^CONFIG_PKG_\([A-Z0-9_]*\)=y$/\1/p' "$L4RE_BUILD/.config.all"); do
        [[ $t != NATIVE_SHELL ]] || continue
        name=$(echo "$t" | tr 'A-Z_' 'a-z-')
        if [[ $name == ada ]]; then goals+=(pkg/ada/libgnat pkg/ada/examples)
        else goals+=("pkg/$name"); fi
    done
    if [[ -n $pkg ]]; then
        extra=$(resolve_package "$pkg")
        if [[ $force == 1 && -d $L4RE_BUILD/$extra ]]; then
            "$MAKE" -C "$PROJ_ROOT/l4mk/$extra" O="$L4RE_BUILD" L4DIR="$PROJ_ROOT/l4mk" clean
        fi
        if [[ $compile_only == 1 ]]; then
            board_make "${parallel[@]}" "$extra"
            return
        fi
        goals+=("$extra")
        local consumers
        consumers=$(python3 "$PROJ_ROOT/tools/build_packages.py" consumers "$L4RE_BUILD" "$extra")
        for name in $consumers; do goals+=("$name"); done
    fi
    if [[ -z $roots ]]; then
        # Upstream example entries have no product-specific root list.
        board_make "${parallel[@]}" all
    else
        board_make "${parallel[@]}" "${goals[@]}"
    fi
}

publish() {
    local src=$1 dst=$2 temp
    [[ -s $src ]] || error "Missing artifact: $src"
    if [[ -f $dst ]] && cmp -s "$src" "$dst"; then return; fi
    mkdir -p "$(dirname "$dst")"
    temp=$(mktemp "$dst.XXXXXX")
    cp -p "$src" "$temp" && mv -f "$temp" "$dst"
}

pack() (
    mkdir -p "$L4RE_BUILD/images"
    local stage
    stage=$(mktemp -d "$L4RE_BUILD/images/.pack.XXXXXX")
    trap 'rm -rf "$stage"' EXIT
    local search="$KERNEL_BUILD:$CONF_DIR"
    if [[ $BOARD == bbb || $BOARD == rpi4 ]]; then
        # Only this named credential file may override the shipped empty store.
        if [[ -f $BUILD_OUT/credentials/$BOARD/board-shadow ]]; then
            ln -s "$BUILD_OUT/credentials/$BOARD/board-shadow" "$stage/board-shadow"
            search="$stage:$search"
        fi
    fi
    board_make BOOTSTRAP_OUTPUT_DIR="$stage" E="$entry" MODULES_LIST="$CONF_DIR/modules.list" \
        MODULE_SEARCH_PATH="$search" elfimage
    [[ -s "$stage/${image##*/}" ]] || error "Image builder produced no ELF for $entry"
    mv -f "$stage/${image##*/}" "$image"
    ln -s "${image##*/}" "$stage/current"
    mv -f "$stage/current" "$L4RE_BUILD/images/bootstrap.elf"
    collect
)
collect() {
    [[ -s $image ]] || error "Build the $BOARD/$entry image first: $image"
    publish "$KERNEL_BUILD/fiasco" "$ARTIFACTS/fiasco"
    publish "$image" "$ARTIFACTS/$entry/bootstrap.elf"
}
case "$target" in
    cleanall) rm -rf "$L4RE_BUILD" "$KERNEL_BUILD" "$ARTIFACTS" ;;
    clean)
        if [[ -d $KERNEL_BUILD ]]; then "$MAKE" -C "$KERNEL_BUILD" clean; fi
        if [[ -d $L4RE_BUILD ]]; then board_make clean; rm -rf "$L4RE_BUILD/images"; fi
        rm -rf "$ARTIFACTS" ;;
    menuconfig|defconfig|oldconfig|olddefconfig|syncconfig|savedefconfig|allnoconfig|allyesconfig) configure ;;
    kernel) init_sources; build_kernel; publish "$KERNEL_BUILD/fiasco" "$ARTIFACTS/fiasco" ;;
    collect) select_entry; collect ;;
    all|l4re|bootstrap|pkg)
        prepare_board
        select_entry
        if [[ $target == all || ( $target != l4re && $compile_only == 0 && ! -s $KERNEL_BUILD/fiasco ) ]]; then build_kernel; fi
        build_product
        if [[ $target != l4re && $compile_only == 0 ]]; then pack; fi ;;
    *) usage >&2; exit 1 ;;
esac
