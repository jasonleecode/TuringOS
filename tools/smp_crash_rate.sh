#!/bin/bash
#
# smp_crash_rate.sh — 量化 Fiasco SMP 调度重入崩溃的发生率（测量工具，非门禁）
# ============================================================================
#
# 背景
#   `context.cpp:*: !schedule_in_progress` 是一个*概率性* SMP 调度重入断言
#   （见 docs/todo.md「已知问题」与 memory/project_p0_sched_crash）。修复这
#   类缺陷必须用**崩溃率**说话，不能用「跑一次过/不过」。本脚本不做门禁
#   （永远 exit 0，除非配置错误），只在 (workload × -smp) 的网格上反复跑、
#   统计每格崩溃率，输出对比矩阵——给「改一版→重测→比崩溃率」的闭环打底。
#
#   与 tools/ci_smp_smoke.sh 的区别：那个是 CI 门禁（有崩即 FAIL）；这个是
#   baseline 量化器（输出 7%、12% 这种数，用于横向比较提交/配置）。
#
# 两种压力源（都专压 schedule() 的 preemption_point() 窗口）
#   spawn — smp-spawn-bench：spawnd 反复 create/destroy 子任务 + 跨核 IPC。
#   login — native_shell 登录提示符下的**击键风暴**：每个字符 = vcon IRQ →
#           唤醒阻塞在 read() 的 stdin_monitor 线程 → 跨核 IPI 重调度。
#           （依赖 main.cc 的「空用户名重新提示」行为，使提示符成为无限压力面。）
#           这正是 2026-06-09 交互登录「打字 roo」时复现的那条触发链。
#
# 判定（每轮）
#   PASS  — 见到该 workload 的存活标志，且无崩溃签名
#   CRASH — 出现 schedule_in_progress / JDB 进入横幅 / Assertion
#   HANG  — 超时内既无存活标志也无崩溃签名（JDB 静默死循环也算）
#
# 用法
#   tools/smp_crash_rate.sh [-n 轮数] [-t 单轮超时] [--smp "2 4"] \
#                           [--workload "spawn login"] [--keep]
#
#   -n N           每格轮数            (默认 50；baseline 建议 100)
#   -t SEC         单轮 wall 超时      (默认按 workload：spawn 30 / login 40)
#   --smp "LIST"   要扫的核数列表       (默认 "2 4"；1 会提示无意义)
#   --workload "L" 压力源列表           (默认 "spawn login")
#   --keep         保留所有日志         (默认仅留失败轮)
#
# 例
#   tools/smp_crash_rate.sh -n 100                 # 全量 baseline（耗时长）
#   tools/smp_crash_rate.sh -n 10 --smp 2 --workload spawn   # 快速试跑
#
# ============================================================================
set -u

PROJ_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG_DIR="$PROJ_ROOT/build/l4re_virt/images"
SPAWN_IMAGE="$IMG_DIR/bootstrap_smp-spawn-bench-ci.elf"
LOGIN_IMAGE="$IMG_DIR/bootstrap_native-shell.elf"

ITERS=50
TIMEOUT=0                 # 0 => 按 workload 自动
SMP_LIST="2 4"
WORKLOADS="spawn login"
KEEP_LOGS=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -n)         ITERS="$2"; shift 2 ;;
        -t)         TIMEOUT="$2"; shift 2 ;;
        --smp)      SMP_LIST="$2"; shift 2 ;;
        --workload) WORKLOADS="$2"; shift 2 ;;
        --keep)     KEEP_LOGS=1; shift ;;
        -h|--help)  sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//; /^set -u/d'; exit 0 ;;
        *) echo "未知参数: $1 (--help 查看用法)"; exit 2 ;;
    esac
done

QEMU=qemu-system-arm
command -v "$QEMU" >/dev/null 2>&1 || { echo "错误: 找不到 $QEMU"; exit 2; }

LOGDIR="$(mktemp -d /tmp/smp_crash_rate.XXXXXX)"
strip_ansi='s/\x1b\[[0-9;]*m//g'

# 崩溃专属签名——绝不含 bench 自己的 "panic" 字样（避免误报）
CRASH_RE='schedule_in_progress|Return reboots, .k. enters|enters the L4 kernel debugger|Assertion failed|: ASSERTION|Cpu#?[0-9]+.*(halt|stuck)'
SPAWN_PASS_RE='PASS.*without kernel panic'
LOGIN_PASS_RE='SMPPROBE_OK'

# ---- spawn 压力源：直接启动 bench 镜像 ------------------------------------
feed_spawn() { :; }   # 无需喂输入

# ---- login 压力源：登录提示符击键风暴 -------------------------------------
# 在提示符下持续敲字符（每个都触发 vcon IRQ + 跨核唤醒），用退格+空回车保持
# 停在 login: 提示符（空用户名→重新提示），最后真正登录并打出存活标志。
feed_login() {
    local stress="$1"          # 击键风暴持续秒数
    sleep 7                    # 等启动到 login 提示符
    local end=$((SECONDS + stress))
    while [ "$SECONDS" -lt "$end" ]; do
        printf 'r'; printf 'o'; printf 'o'; printf 't'
        printf '\177\177\177\177'      # 4 个退格，擦回空
        printf '\n'                    # 空用户名 → 重新提示（不会误登录）
        sleep 0.02
    done
    printf '\n\n'; sleep 0.3           # 冲掉可能的半截输入
    printf 'root\n';      sleep 0.5
    printf '12345678\n';  sleep 1
    printf 'echo SMPPROBE_OK\n'; sleep 2
    # 再补一发，防止前面落在某次失败重试里
    printf 'root\n12345678\n'; sleep 1
    printf 'echo SMPPROBE_OK\n'; sleep 2
}

# ---- 单轮 -----------------------------------------------------------------
# 入参: workload smp log ；返回 0=PASS 1=CRASH 2=HANG；诊断写 $REASON
run_one() {
    local wl="$1" smp="$2" log="$3"
    REASON=""
    local image mem pass_re tmo netdev=()
    case "$wl" in
        spawn) image="$SPAWN_IMAGE"; mem=48M;  pass_re="$SPAWN_PASS_RE"
               tmo=$([ "$TIMEOUT" -gt 0 ] && echo "$TIMEOUT" || echo 30) ;;
        login) image="$LOGIN_IMAGE"; mem=256M; pass_re="$LOGIN_PASS_RE"
               tmo=$([ "$TIMEOUT" -gt 0 ] && echo "$TIMEOUT" || echo 40)
               netdev=(-netdev user,id=net0 -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0) ;;
    esac

    # 输入源：spawn 不喂；login 喂击键风暴（风暴时长 = 超时 - 启动/登录余量）
    if [ "$wl" = "login" ]; then
        "$QEMU" -M virt -cpu cortex-a15 -m "$mem" -smp "$smp" \
            -kernel "$image" -display none -serial stdio -no-reboot \
            "${netdev[@]}" < <(feed_login $(( tmo - 18 ))) > "$log" 2>&1 &
    else
        "$QEMU" -M virt -cpu cortex-a15 -m "$mem" -smp "$smp" \
            -kernel "$image" -display none -serial stdio -no-reboot \
            < /dev/null > "$log" 2>&1 &
    fi
    local pid=$!

    local waited=0 result=2 txt
    while [ "$waited" -lt "$tmo" ]; do
        kill -0 "$pid" 2>/dev/null || break
        txt="$(sed "$strip_ansi" "$log" 2>/dev/null)"
        if grep -qaE "$CRASH_RE" <<<"$txt"; then
            REASON="$(grep -aoE "$CRASH_RE" <<<"$txt" | head -1)"; result=1; break
        fi
        if grep -qaE "$pass_re" <<<"$txt"; then result=0; break; fi
        sleep 1; waited=$((waited + 1))
    done

    kill -TERM "$pid" 2>/dev/null
    for _ in 1 2 3; do kill -0 "$pid" 2>/dev/null || break; sleep 0.3; done
    kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null

    if [ "$result" -eq 2 ]; then        # 超时未判定，最终再判一次
        txt="$(sed "$strip_ansi" "$log" 2>/dev/null)"
        if grep -qaE "$CRASH_RE" <<<"$txt"; then
            REASON="$(grep -aoE "$CRASH_RE" <<<"$txt" | head -1)"; result=1
        elif grep -qaE "$pass_re" <<<"$txt"; then result=0
        else REASON="超时 ${tmo}s 无存活标志（疑似 JDB 静默死循环）"; fi
    fi
    return "$result"
}

# ---- 主循环：扫 workload × smp 网格 ---------------------------------------
echo "=================================================="
echo " SMP 调度崩溃率 baseline 量化"
echo "=================================================="
echo " 每格轮数 : $ITERS"
echo " 核数列表 : $SMP_LIST"
echo " 压力源   : $WORKLOADS"
echo " 日志目录 : $LOGDIR"
echo "--------------------------------------------------"

declare -a SUMMARY=()
for wl in $WORKLOADS; do
    img_var="$([ "$wl" = spawn ] && echo "$SPAWN_IMAGE" || echo "$LOGIN_IMAGE")"
    if [ ! -f "$img_var" ]; then
        echo "  [$wl] 跳过：找不到镜像 $img_var（请先 ./build.sh --board virt all）"
        continue
    fi
    for smp in $SMP_LIST; do
        pass=0; crash=0; hang=0
        echo "  >>> workload=$wl  -smp $smp"
        for i in $(seq 1 "$ITERS"); do
            log="$LOGDIR/${wl}_smp${smp}_$(printf '%03d' "$i").log"
            printf "      [%3d/%3d] " "$i" "$ITERS"
            start=$(date +%s)
            run_one "$wl" "$smp" "$log"; rc=$?
            dur=$(( $(date +%s) - start ))
            case "$rc" in
                0) pass=$((pass+1));  printf "PASS  (%2ds)\n" "$dur"
                   [ "$KEEP_LOGS" -eq 0 ] && rm -f "$log" ;;
                1) crash=$((crash+1));printf "CRASH (%2ds)  %s\n" "$dur" "$REASON" ;;
                2) hang=$((hang+1)); printf "HANG  (%2ds)  %s\n" "$dur" "$REASON" ;;
            esac
        done
        local_fail=$((crash + hang))
        rate=$(awk "BEGIN{printf \"%.1f\", 100.0*$local_fail/$ITERS}")
        SUMMARY+=("$(printf '%-7s smp=%-2s  崩溃率=%5s%%  (CRASH=%d HANG=%d PASS=%d / %d)' \
                    "$wl" "$smp" "$rate" "$crash" "$hang" "$pass" "$ITERS")")
        printf "      ---- %s smp=%s: 崩溃率 %s%% (CRASH=%d HANG=%d PASS=%d)\n" \
               "$wl" "$smp" "$rate" "$crash" "$hang" "$pass"
    done
done

echo "=================================================="
echo " 崩溃率矩阵（越高越易复现；改内核后重跑同参对比）"
echo "--------------------------------------------------"
for s in "${SUMMARY[@]}"; do echo "  $s"; done
echo "=================================================="
echo " 失败日志保留于: $LOGDIR"
echo "（这是测量工具，不作门禁；exit 0）"
exit 0
