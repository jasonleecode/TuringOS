# TuringOS 性能测试体系规划

> 编写日期：2026-05-14  更新：2026-05-15
> 当前实现测量固定周期的唤醒延迟、IPC 往返和吞吐。双核压力测试还校验子任务退出和跨核 IPC 的实际完成次数。

---

## 一、开源 cyclictest（rt-tests）功能全集

Linux rt-tests 套件中的 cyclictest 是实时系统调度延迟测试的工业标准工具，其完整功能如下：

### 1.1 核心测量

| 功能 | 说明 |
|------|------|
| 定时器类型 | `clock_nanosleep`（默认）、POSIX interval timer（-s）、nanosleep（-n） |
| 时钟源选择 | `CLOCK_MONOTONIC`、`CLOCK_REALTIME`、`CLOCK_BOOTTIME`、`CLOCK_TAI`（-c N） |
| 精度 | **纳秒级**（测量值、统计、直方图均为 ns） |
| 唤醒模式 | 标准模式（sleep→wakeup延迟）、触发模式（IRQ→线程延迟）（-T） |
| 定时器分辨率 | 独立分辨率测量模式（-r），报告 POSIX `clock_getres()` 实测值 |

### 1.2 线程与调度

| 功能 | 说明 |
|------|------|
| 线程数 | `-t N` |
| 调度策略 | SCHED_FIFO（-f）、SCHED_RR（-r）、SCHED_OTHER（默认）、SCHED_DEADLINE（-E） |
| 线程优先级 | `-p N`（SCHED_FIFO 最高 99） |
| CPU 亲和性 | `-a [CPU]` 绑核，`-A [CPU]` 主线程绑核，支持 CPU 掩码（如 0x3） |
| 线程间距 | `-d N`：各线程 interval = base + N×id（错开唤醒避免同时竞争） |
| 内存锁定 | `-m`（mlockall），防止测量过程中触发 page fault |
| 预热阶段 | `-W N`：前 N 次循环不计入统计（消除 cold-cache 影响） |

### 1.3 运行控制

| 功能 | 说明 |
|------|------|
| 循环次数 | `-l N` |
| 运行时长 | `-D N`（秒），与 -l 互斥 |
| 超时中止 | `-b N`：延迟超过阈值立即停止并输出 trace（配合 ftrace） |
| 安静模式 | `-q`：不输出每秒刷新行，只输出最终结果 |
| 详细模式 | `-v`：每次迭代打印延迟值（用于细粒度分析） |

### 1.4 统计与输出

| 功能 | 说明 |
|------|------|
| 基础统计 | min / avg / max（per-thread + 汇总） |
| **百分位** | p50 / p95 / p99 / p99.9 / p99.99 |
| **标准差** | 抖动量化 |
| 直方图 | `-H N`：可配置桶数，ns 精度，支持 overflow 桶 |
| 文件输出 | `-o FILE` |
| JSON 输出 | `--json`（机器可读，便于 CI 对比） |
| 对齐时钟 | `--secaligned N`：在整秒边界启动，方便多机对比 |

### 1.5 平台专项（x86）

- `--smi`：检测 SMI（系统管理中断）对延迟的影响（x86 BIOS 干扰）
- ftrace 集成：超阈值时自动抓取内核 trace（需 CONFIG_FTRACE）

---

## 二、我们当前实现 vs 开源 cyclictest

### 2.1 已实现 ✓

| 功能 | 备注 |
|------|------|
| 周期唤醒延迟 | 以 KIP 单调时钟设定绝对截止时间，记录迟到时间及跳过的周期数 |
| min / avg / max（per-thread + ALL） | |
| 读钟 | 使用 `l4_kip_clock_ns`，以 µs 显示三位小数。实际分辨率取决于平台 |
| **百分位统计**（p50/p90/p99/p99.9） | 按最近秩法向上取整。首末秩取实测极值，普通桶取近似值，无法确定的溢出分位显示下界 |
| 总体标准差 | 使用 Welford 在线算法，汇总时合并各线程的样本数、均值和方差 |
| 1000 桶直方图（1µs/桶，overflow ≥999µs） | 单线程及汇总使用同一组桶边界 |
| 多线程（pthread，最多 8 个） | |
| 运行时可配置参数（-i/-l/-t/-p/-b） | `run cyclictest -t 2 -l 300 -i 500` 直接生效，无需重建镜像（2026-05-15 修复 spawnd UTCB 覆写 bug + readline 崩溃） |
| `-b N` 延迟上限 | 完成采样后输出超阈值次数。N 为正且有超限时退出 1；默认 0 表示仅测量 |

### 2.2 缺失功能（按重要性排序）

#### 高优先级——影响数据可信度

| 缺失项 | 影响 | 实现难度 |
|--------|------|----------|
| **CPU 亲和性（线程绑核）** | 8 线程 / 2 核 过度订阅使 max 虚高，绑核后结果才有对比意义 | 低（`run_thread` 加 affinity 参数） |

#### 中优先级——完整性

| 缺失项 | 说明 |
|--------|------|
| **Duration 模式**（-D 秒） | 连续压测时比固定循环次数更实用 |
| **线程间距**（-d N） | 错开各线程唤醒时刻，更接近实际多任务场景 |
| **预热阶段**（-W N） | 消除首次 cache miss 对 min 的影响 |
| **可配置直方图桶宽** | 当前固定 1µs/桶，高精度场景下需要更细粒度 |

#### 低优先级——工程便利性

| 缺失项 | 说明 |
|--------|------|
| JSON / 文件输出 | CI 系统对比历史数据时需要 |
| 安静 / 详细模式（-q / -v） | 长时间测试时 -q 避免刷屏 |
| 多次运行自动对比 | 类 regression test |

---

## 三、系统级完整性能测试体系

cyclictest 只覆盖调度延迟这一个维度。一个完整的实时 OS 性能测试体系应覆盖以下层次：

### 3.1 实时性（Temporal Determinism）

| 测试 | 工具方向 | TuringOS 现状 |
|------|----------|---------------|
| 调度延迟（wakeup latency） | cyclictest | ✓ 基础版 |
| 中断响应延迟（IRQ→线程） | irqlatency / cyclictest -T | ✗ |
| 时钟精度与漂移 | clock_getres + long-run drift test | ✗ |
| 优先级反转检测 | prio-inversion test | ✗ |
| 最坏情况执行时间（WCET） | 需要与负载生成器配合 | ✗ |

**为什么重要**：调度延迟好看不代表在实际负载下仍然好看。IRQ 延迟和优先级反转是导致实时系统超限的两大根因。

### 3.2 IPC 性能（核心通信路径）

| 测试 | 指标 | TuringOS 现状 |
|------|------|---------------|
| IPC 往返延迟（round-trip） | µs | ✓ ipcbench（见下节） |
| 跨核 IPC 延迟 vs 同核 | µs，需 SMP | ✓ ipcbench lat 模式自动对比 |
| IPC 吞吐（消息/秒） | msg/s | ✓ ipcbench thru/hack 模式 |
| IPC 半往返耗时估算 | µs | ✓ ipcbench ctx 模式（RTT/2，含 IPC 与调度开销） |
| hackbench（N 对并发） | msg/s | ✓ ipcbench hack 模式 |
| 大消息传输带宽 | MB/s（shared-DS 路径） | ✗ 待做 |

**为什么重要**：L4Re 所有服务调用都是 IPC，IPC 延迟直接决定系统调用和驱动访问的代价。

### 3.3 调度器吞吐（Scheduler Throughput）

| 测试 | 说明 | 参考工具 |
|------|------|---------|
| 上下文切换时间 | 同优先级线程切换开销 | lmbench lat_ctx |
| 任务创建 / 销毁开销 | thread create + join 时间 | 自研 |
| 高并发调度吞吐 | N 个线程互发 IPC 的总吞吐 | hackbench 等价物 |

**为什么重要**：低延迟 ≠ 高吞吐。Fiasco 的优先级调度在大量低优先级线程下有不同的行为。

### 3.4 内存性能

| 测试 | 指标 | 参考工具 |
|------|------|---------|
| 内存带宽（读 / 写 / 拷贝） | GB/s | stream benchmark |
| Cache 层级延迟（L1/L2/主存） | ns | lat_mem_rd |
| 内存分配延迟 | µs/alloc | malloc bench |
| 大页 / 对齐访问收益 | — | 自研 |

**为什么重要**：嵌入式场景内存带宽直接影响视频处理、AI 推理等数据密集型任务。

### 3.5 存储 I/O 性能

| 测试 | 指标 | TuringOS 现状 |
|------|------|---------------|
| VirtIO-blk 顺序读写 | MB/s | ✗ |
| ext4 随机读写 IOPS | ops/s | ✗ |
| 文件系统元数据操作 | ops/s（open/close/stat） | ✗ |

**为什么重要**：当前 ext4 实现有 4MiB 文件上限等缺陷，没有量化指标就无法判断何时需要优化。

### 3.6 网络性能

| 测试 | 指标 | TuringOS 现状 |
|------|------|---------------|
| TCP 吞吐 | MB/s（iperf 等价） | ✗ 只有 echo server |
| UDP 丢包率 vs 速率 | pps | ✗ |
| TCP RTT | µs | ✗ |
| 协议栈处理开销 | per-packet CPU % | ✗ |

### 3.7 CPU 计算性能

| 测试 | 说明 |
|------|------|
| CoreMark / Dhrystone | 整数指令流水线 |
| Whetstone / LINPACK | 浮点 / SIMD |
| 加解密（AES/SHA） | 安全场景基准 |

**注**：这类 benchmark 移植简单（纯计算，无 OS 依赖），但 QEMU 上结果没有参考价值，需在真实硬件（BBB / RPi4）上跑。

### 3.8 系统综合指标

| 测试 | 说明 |
|------|------|
| 冷启动时间 | bootstrap → 第一个用户程序输出 |
| 内存基线占用 | 空载时 kernel + l4re + ned 占用 |
| 多服务并发稳定性 | 长时间运行（24h）无崩溃 |

---

## 三点五、ipcbench 设计方案

> 编写日期：2026-05-15

### 定位

一个二进制，4 种测试模式，覆盖 IPC 往返、半往返估算、单对和多对吞吐。

```
run ipcbench              # 默认：lat 模式（同核 + 跨核自动对比）
run ipcbench -m ctx       # IPC 半往返耗时估算
run ipcbench -m thru      # 单对吞吐
run ipcbench -m hack -n 8 # hackbench（8 对并发）
```

### 模式说明

#### lat — IPC 往返延迟（核心）

```
client: t0 = kip_ns(); l4_ipc_call(gate); t1 = kip_ns(); lat = t1 - t0
server: l4_ipc_wait() → loop: l4_ipc_reply_and_wait()
```

自动运行两轮，无需手动指定 CPU：
- Round 1：client=CPU0，server=CPU0（同核）
- Round 2：client=CPU0，server=CPU1（跨核）
- 输出 overhead 绝对值与百分比

#### ctx — IPC 半往返耗时估算

与 lat 使用相同路径，报告 RTT/2，包含内核收发、线程调度和用户态读钟开销。

#### thru — 单对吞吐

固定时长（`-D 秒`）内 client 全速 `l4_ipc_call()`，统计 Kcalls/s。
不做延迟直方图（计时开销会拖低吞吐数字）。

#### hack — N 对并发（hackbench 等价）

创建 N 对 (client, server) pthread，每对使用独立 gate。全部预热后共同起跑，只统计统一截止时间内收到有效应答的往返，单位为 calls/s。
压调度器吞吐极限。

### 参数

| 参数 | 说明 | 默认 |
|------|------|------|
| `-m <lat\|ctx\|thru\|hack>` | 测试模式 | lat |
| `-i <n>` | 迭代次数（lat/ctx） | 10000 |
| `-D <s>` | 运行时长秒（thru/hack） | 3 |
| `-n <n>` | 并发对数（hack） | 4 |
| `-p <prio>` | 线程优先级 | 10 |

### 关键实现

每个服务端线程启动并设置调度参数后，直接创建绑定到该线程的 gate。客户端使用有界超时并校验应答。任意线程创建、调度、IPC 或退出校验失败均返回非零。

延迟和标准差由 `pkg/benchmark/latency_stats.h` 统一计算。在宿主机可运行确定性统计回归：

```sh
c++ -std=c++17 pkg/benchmark/tests/latency_stats_test.cc -o /tmp/turing-latency-test
/tmp/turing-latency-test
```

`tools/ci_smp_smoke.sh` 每轮检查 SMP 标记、各 spawner 的最少成功退出数、成功跨核应答数和错误数。

**SMP 检测**：跑 cross-core 前查询 `l4_scheduler_info()`，CPU1 不可用则 skip。

### 文件结构

```
pkg/benchmark/ipcbench/
├── Control          (requires: stdlibs libstdc++ l4re l4re_c l4util)
├── Makefile
└── src/
    ├── Makefile     (REQUIRES_LIBS += libpthread pthread-l4)
    └── ipcbench.cc  (~400 行)

conf/ipcbench.cfg
conf/modules.list    (新增 entry ipcbench)
```

### 预期输出（lat 模式）

```
=== ipcbench results ===
  mode       : lat  iterations=10000  priority=10

  same-core  (client=CPU0  server=CPU0):
    min=0.800 us  avg=1.856 us  max=5.432 us  stddev=0.312 us
    p50=1.800 us  p90=2.100 us  p99=3.200 us  p99.9=4.500 us

  cross-core (client=CPU0  server=CPU1):
    min=1.200 us  avg=3.012 us  max=8.765 us  stddev=0.521 us
    p50=2.900 us  p90=3.500 us  p99=5.200 us  p99.9=7.800 us

  cross-core overhead: +1.156 us avg (+62%)
========================
```

---

## 四、优先级与实施路线

### 近期（1–2 周）：完善 cyclictest 本身

- [✓] ~~运行时可配置参数~~（已完成，2026-05-15）
- [✓] ~~ns 精度 + 1000桶直方图~~（已完成，2026-05-15）
- [✓] ~~百分位统计（p50/p90/p99/p99.9）~~（已完成，2026-05-15）
- [✓] ~~标准差~~（已完成，2026-05-15）
1. **加 CPU affinity**：`-a` 参数，`l4_sched_param_t` 的 `affinity` 字段，2 核下可测绑核 vs 不绑核差异
2. **加 -D duration 模式**：用 KIP clock 判断运行时长代替固定循环次数

### 中期（1 个月）：IPC + 调度专项

5. **跨核 IPC 延迟测试**：基于现有 ipcbench，加 SMP 亲和性对比
6. **上下文切换时间**：两线程通过 IPC 乒乓，测 round-trip / 2
7. **hackbench 等价物**：N 对线程全速 IPC，测总 msg/s

### 长期（上硬件后）：完整测试套件

8. **内存带宽**（stream）、**存储 IOPS**、**网络吞吐**
9. **真实硬件 cyclictest**：BBB / RPi4 裸机结果才有与 Linux RT 的可比性
10. **长时间稳定性测试**：24 小时 cyclictest，监控 max 趋势

---

## 五、参考对比基线

在真实 ARM 裸机 + Fiasco.OC 下，预期可达到的指标（供后续验证参考）：

| 指标 | 目标值（裸机单线程，SCHED_FIFO 等价优先级） |
|------|------|
| cyclictest min | < 10 µs |
| cyclictest avg | < target + 5 µs |
| cyclictest max | < 100 µs（无外部负载） |
| IPC round-trip | < 2 µs（同核）/ < 5 µs（跨核） |
| 上下文切换 | < 3 µs |

QEMU 上的数值仅用于功能验证和回归对比，不具备绝对性能参考意义。
