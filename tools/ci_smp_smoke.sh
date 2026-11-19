#!/bin/bash
#
# ci_smp_smoke.sh — SMP 调度器崩溃回归冒烟测试 (CI gate)
# ============================================================================
#
# 背景
#   Fiasco 在 SMP (-smp 2) 下存在偶发断言崩溃：
#       context.cpp:758: !schedule_in_progress
#   触发后 CPU 进入 JDB 死循环、系统挂起。这是个*概率性*缺陷
#   （见 docs/todo.md「已知问题」），单次运行通过不能证明已修复——
#   必须多轮运行并统计崩溃率。
#
# 做法
#   反复在 QEMU 无显示模式下启动 `smp-spawn-bench` entry（spawnd 反复
#   create/destroy 子任务 + 跨核 IPC 噪声，专门压 preemption_point() 窗口）。
#   每轮判定：
#     PASS   — 双核负载完成，每个 spawner 成功退出子任务，跨核 IPC 收到有效应答
#     CRASH  — 出现 schedule_in_progress / JDB 进入横幅（提前退出）
#     HANG   — 超时内既无 PASS 也无崩溃签名（JDB 静默死循环也算失败）
#     MISCFG — bench 报告 SMP=no（测试无意义，按失败处理）
#
#   所有轮次 PASS → 退出码 0；否则非 0（CI 失败）。
#
# 注意（避免误报）
#   bench 自身会打印 "kernel panic = fix regressed" 和 "without kernel
#   panic"，因此同时校验 PASS 行和负载计数；崩溃签名只用专属 token
#   （schedule_in_progress / JDB 横幅），绝不用裸 "panic"。
#
# 用法
#   tools/ci_smp_smoke.sh [-n 轮数] [-t 单轮超时秒] [--smp N] [--image PATH]
#
#   -n N        迭代轮数            (默认 10)
#   -t SEC      单轮 wall 超时秒    (默认 30；CI 镜像 bench -D 8 + 启动余量)
#   --smp N     QEMU CPU 核心数     (默认 2；崩溃只在 >=2 复现)
#   --image P   引导镜像路径        (默认 bootstrap_smp-spawn-bench-ci.elf；
#                                    长压换 bootstrap_smp-spawn-bench.elf 并加大 -t)
#   --keep      保留所有轮次日志    (默认仅保留失败轮次)
#
# CI 示例
#   tools/ci_smp_smoke.sh -n 20          # 空目录构建专用镜像后测试
#
# ============================================================================
set -u

PROJ_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

ITERS=10
TIMEOUT=30
SMP=2
# 默认用 CI 专用短时长镜像（smp-spawn-bench-ci entry: -D 8 -s 3），单轮 ~10s，
# 单位 wall time 样本数约为 30s 版的 3 倍。需要长压可 --image 换回完整版。
IMAGE=""
KEEP_LOGS=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -n)        ITERS="$2"; shift 2 ;;
        -t)        TIMEOUT="$2"; shift 2 ;;
        --smp)     SMP="$2"; shift 2 ;;
        --image)   IMAGE="$2"; shift 2 ;;
        --keep)    KEEP_LOGS=1; shift ;;
        -h|--help)
            sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//; /^set -u/d'
            exit 0 ;;
        *) echo "未知参数: $1 (--help 查看用法)"; exit 2 ;;
    esac
done

# ---- 前置检查 -------------------------------------------------------------
if ! [[ "$ITERS" =~ ^[1-9][0-9]*$ ]]; then
    echo "错误: -n 需要正整数" >&2
    exit 2
fi
QEMU=qemu-system-arm
command -v "$QEMU" >/dev/null 2>&1 || { echo "错误: 找不到 $QEMU"; exit 2; }
if [[ -z $IMAGE ]]; then
    build_dir=$(mktemp -d "${TMPDIR:-/tmp}/turingos-smp.XXXXXX") || exit 2
    trap 'rm -rf "$build_dir"' EXIT
    BUILD_OUT="$build_dir" ENTRY=smp-spawn-bench-ci "$PROJ_ROOT/build.sh" --board virt all || exit 2
    IMAGE="$build_dir/artifacts/virt/smp-spawn-bench-ci/bootstrap.elf"
fi
[[ -s $IMAGE ]] || { echo "找不到测试镜像 $IMAGE" >&2; exit 2; }
if [ "$SMP" -lt 2 ]; then
    echo "警告: --smp $SMP < 2，该崩溃只在多核复现，测试将无意义。"
fi

LOGDIR="$(mktemp -d /tmp/ci_smp_smoke.XXXXXX)"

# ANSI 去色后匹配（串口输出带颜色码，关键 token 不含颜色码但保险起见剥离）
strip_ansi='s/\x1b\[[0-9;]*m//g'

PASS_RE='PASS.*without kernel panic'
SMP_OK_RE='SMP=yes'
WORK_RE='SMP_WORK spawners=[1-9][0-9]* min_spawns=[1-9][0-9]* ipc_ok=[1-9][0-9]* errors=0[[:space:]]*$'
FAIL_RE='SMP=no|\[benchmark\] FAIL'
# 崩溃专属签名——绝不含 bench 自己的 "panic" 字样
CRASH_RE='schedule_in_progress|Return reboots, .k. enters|enters the L4 kernel debugger|Assertion failed|: ASSERTION|Cpu#?[0-9]+.*(halt|stuck)'

# ---- 单轮运行 -------------------------------------------------------------
# 返回: 0=PASS 1=CRASH 2=HANG 3=MISCFG；诊断写入全局 $REASON
run_one() {
    local idx="$1"
    local log="$LOGDIR/iter_$(printf '%02d' "$idx").log"
    REASON=""
    LASTLOG="$log"

    "$QEMU" -M virt -cpu cortex-a15 -m 48M -smp "$SMP" \
        -kernel "$IMAGE" -display none -serial stdio -no-reboot \
        < /dev/null > "$log" 2>&1 &
    local pid=$!

    local waited=0 result=2
    while [ "$waited" -lt "$TIMEOUT" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            # QEMU 自行退出（一般不会，系统会 idle）；按当前日志判定
            break
        fi
        local txt
        txt="$(sed "$strip_ansi" "$log" 2>/dev/null)"
        if grep -qE "$CRASH_RE|$FAIL_RE|$PASS_RE" <<<"$txt"; then break; fi
        sleep 1
        waited=$((waited + 1))
    done

    # 收尾：杀掉 QEMU
    kill -TERM "$pid" 2>/dev/null
    for _ in 1 2 3; do kill -0 "$pid" 2>/dev/null || break; sleep 0.3; done
    kill -KILL "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null

    # 正常退出和超时都使用相同判据，崩溃优先。
    local txt
    txt="$(sed "$strip_ansi" "$log")"
    if grep -qE "$CRASH_RE" <<<"$txt"; then
        REASON="$(grep -aoE "$CRASH_RE" <<<"$txt" | head -1)"; result=1
    elif grep -qE "$FAIL_RE" <<<"$txt"; then
        REASON="负载失败或缺少在线 CPU"; result=3
    elif grep -qE "$PASS_RE" <<<"$txt"; then
        if grep -qE "$SMP_OK_RE" <<<"$txt" && grep -qE "$WORK_RE" <<<"$txt"; then
            result=0
        else
            REASON="缺少双核、各 spawner 或跨核 IPC 的完成证据"; result=3
        fi
    else
        REASON="未收到测试完成标记"; result=2
    fi
    return "$result"
}

# ---- 主循环 ---------------------------------------------------------------
echo "=========================================="
echo " SMP 调度崩溃冒烟测试"
echo "=========================================="
echo " 镜像   : $IMAGE"
echo " 轮数   : $ITERS    单轮超时: ${TIMEOUT}s    -smp $SMP"
echo " 日志   : $LOGDIR"
echo "------------------------------------------"

pass=0; crash=0; hang=0; miscfg=0
declare -a FAILED_LOGS=()

for i in $(seq 1 "$ITERS"); do
    printf "  [%2d/%2d] " "$i" "$ITERS"
    start=$(date +%s)
    run_one "$i"; rc=$?
    dur=$(( $(date +%s) - start ))
    case "$rc" in
        0) pass=$((pass+1));   printf "PASS   (%2ds)\n" "$dur" ;;
        1) crash=$((crash+1)); printf "CRASH  (%2ds)  %s\n" "$dur" "$REASON"; FAILED_LOGS+=("$LASTLOG") ;;
        2) hang=$((hang+1));   printf "HANG   (%2ds)  %s\n" "$dur" "$REASON"; FAILED_LOGS+=("$LASTLOG") ;;
        3) miscfg=$((miscfg+1));printf "MISCFG (%2ds)  %s\n" "$dur" "$REASON"; FAILED_LOGS+=("$LASTLOG") ;;
    esac
    # 通过的轮次默认删日志，省空间
    if [ "$rc" -eq 0 ] && [ "$KEEP_LOGS" -eq 0 ]; then rm -f "$LASTLOG"; fi
done

fail=$((crash + hang + miscfg))
echo "------------------------------------------"
printf " 结果: %d/%d PASS" "$pass" "$ITERS"
[ "$crash" -gt 0 ]  && printf "  | CRASH=%d" "$crash"
[ "$hang" -gt 0 ]   && printf "  | HANG=%d" "$hang"
[ "$miscfg" -gt 0 ] && printf "  | MISCFG=%d" "$miscfg"
echo ""
if [ "$fail" -gt 0 ]; then
    # 概率性缺陷：报告崩溃率，便于横向比较不同提交
    rate=$(awk "BEGIN{printf \"%.1f\", 100.0*$fail/$ITERS}")
    echo " 失败率: ${rate}%  ($fail/$ITERS)"
    echo " 失败日志:"
    for l in "${FAILED_LOGS[@]}"; do echo "   $l"; done
    echo "=========================================="
    echo "FAIL — SMP 基准未通过，原因见各轮结果"
    exit 1
fi

# 全过：清理空日志目录
[ "$KEEP_LOGS" -eq 0 ] && rmdir "$LOGDIR" 2>/dev/null
echo "=========================================="
echo "PASS — $ITERS 轮全部存活，未触发调度断言"
exit 0
