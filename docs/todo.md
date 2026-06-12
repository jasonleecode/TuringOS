# TuringOS 待办事项

> 按优先级排列，`[✓]` 表示已完成。

---

## P0 — 核心基础

| 状态 | 任务 |
|------|------|
| [✓] | 用户态 Shell（native_shell，含 readline、历史记录、后台 `&` 运算符） |
| [✓] | 构建系统包发现机制（Control 文件、PKGDIR 层级、sync_pkg_symlinks） |
| [✓] | 多启动项配置（modules.list 已有 native-shell / tcp-server / fb-test / fb-drv / lvgl-demo / uart-test） |
| [✓] | TCP echo server（lwip + virtio-net，QEMU NAT 转发验证通过） |
| [✓] | SMP 双核默认（run_qemu_virt.sh -smp 2，smp-test PASSED） |
| [✓] | klog 日志系统（dmesg 命令，ring buffer，severity 过滤） — 升级为系统级 libklog（见下注） |
| [✓] | uptime 命令（CLOCK_MONOTONIC，天/时/分/秒格式） |
| [✓] | i.MX6UL（Cortex-A7）平台适配（QEMU mcimx6ul-evk，CONFIG_CPU_VIRT 已禁用） |
| [✓] | 终端登录（`login:` / `Password:` 提示，密码显示 `*`，默认 root/12345678） |
| [✓] | 构建工具链升级：virt/bbb/imx6ul 统一使用 GCC 12（上游要求 GCC 11+） |
| [✓] | 时区支持：`gmtime()` 改为 `localtime()`，cfg 传 `TZ=CST-8`（POSIX 格式，可按板子改） |
| [✓] | `top` 命令（htop 风格）：termbox2 TUI，spawnd CPU%，uptime，内存，颜色；`q` 退出 |

> **run_qemu_virt.sh**：ARM virt 内存已调整为 48M（原 512M），验证最低内存需求。


---

## P0.5 — 上游同步 [✓]

### 同步 l4re / kernel / l4mk fork 分支

已完成三个子模块同步（2026-05-06），无冲突，构建验证通过：

| 子模块 | 上游 | 同步 commit 数 | 自定义 commit 数 |
|--------|------|----------------|-----------------|
| kernel (Fiasco) | kernkonzept/fiasco r-2026-W17 | +354 | 1（BBB 模板） |
| l4re | kernkonzept/l4re-core | +140 | 1（VFS getdents） |
| l4mk | kernkonzept/mk | +51 | 19（启动配置） |

**注**：上游 Fiasco r-2026-W17 包含 ARM generic timer one-shot 修复，缓解了
SMP 双核下 `schedule_in_progress` 断言偶发崩溃（context.cpp:758）。该根因为
Fiasco 已知设计缺陷，上游暂无直接补丁。

**后续**：建立定期同步机制（建议每月跟一次上游 tag）。

---

## 架构审视（2026-05-08）

> 以下是对当前系统完整度的分析，作为后续开发路线图的依据。

### 当前架构全景

```
Fiasco.OC (微内核)
    └── L4Re (运行时框架)
         ├── ned (init/loader)          ← 静态配置，无服务监督
         ├── io (设备总线)
         ├── ext4fs (文件系统服务)       ← 流式读写（任意大小）+ mkdir/unlink + cap 生命周期 + 原子 O_APPEND
         ├── rtc / virtio-block / virtio-net / fb-drv (驱动)
         ├── native_shell (交互式 shell) ← 无管道、无脚本、无 PATH
         └── lvgl-demo / wamr / uart-test (应用)
```

### 六大核心缺口

> **进度小结（2026-06-09 复盘）**：缺口 1（进程生命周期，含 exec，**完整闭合**）、缺口 3（文件系统，含 /proc /sys，**完整闭合**）、
> 缺口 5（日志，仅缺轮转）已**闭合 / 基本闭合**。仍**敞着的结构性缺口**是 **2（管道/IPC 通道）**、
> **4（Shell 仍是命令分发器）**、**6（安全模型：密码硬编码 + `run` 全量转发 cap 含 sigma0 + 无隔离）**。
> 跨越所有方向的 **P0 调度概率崩溃已于 2026-06-09 修复**（根因＝preemption_point 抢占切走后留下的陈旧
> `schedule_in_progress` 标志；`kernel/` context.cpp 修，login 风暴复现率 100%→0%）——RT/工业方向的硬闸门已拆。

#### 缺口 1：进程生命周期（最关键）

`run` 命令能 spawn 任务，生命周期管理**已完成**：

- [✓] **exit/wait**：`wait <handle>` 阻塞直到子任务退出，`_table.free()` 通过 `Unique_del_cap` RAII 销毁 task/thread/rm/gate 全部内核对象，无 cap 泄漏
- [✓] **kill**：`kill <handle>` 强制销毁子任务所有内核对象并回收 cap slot
- [✓] **孤儿清理**：spawnd 专用 reaper pthread，所有 parent_gate 绑到该线程；子进程退出 IPC 在此收取并标记 `EXITED`（唤醒 `do_wait`），500ms 接收超时轮询做崩溃检测，子进程崩溃未调 `_exit` 也能回收资源（commit a6cf922 / 8b7096a）
- [✓] **exec 语义（2026-06-08）**：`Spawn_svr::exec(handle,path,args)` —— 子进程经转发的 `spawnd` cap
  回调（自身 handle 从 env `SPAWND_HANDLE` 得），spawnd 用新 ELF 起新 task 装回**同一 slot/handle**、
  销毁旧 task（调用者 thread 随之消失，返回 `-L4_ENOREPLY` 不回复）。handle 不变 → 父 `wait`/`kill` 透明。
  这是本系统进程模型下的 exec 语义（底层 L4 task 身份变，但 handle 才是用户可见的"PID"）。
  QEMU 验证：`run exectest &` → 打 `before exec (handle=1)`，随即 spawnd `exec: handle=1 now runs rom/hello`，
  之后持续 `Hello World!`（同一 handle 运行新程序），无崩溃。`pkg/exectest` 为演示/自测程序。
  **后续**：透明 libc `execve()`/`execvp()` shim（让任意 POSIX 程序的 exec() 自动走此机制）；
  转发 spawnd 自身 cap 给子进程，与 [[缺口 6]] all-cap 转发问题相关，细化 cap 策略时一并收。

#### 缺口 2：管道与 IPC 通道

Shell 完全不支持 `cmd1 | cmd2`。根本原因：

- L4Re 无 POSIX `pipe()`（注释中已标注）
- 所有 IPC 都是同步 L4Re Call，没有字节流通道抽象
- 没有 Unix domain socket

没有管道，Unix 哲学无从谈起。设计方向：基于共享 Dataspace + 读写索引实现环形字节流，封装成 `pipe()` 兼容接口。

#### 缺口 3：文件系统不完整

| 功能 | 状态 | 影响 |
|------|------|------|
| mkdir / unlink | [✓] | `mkdir` / `rm` 命令经 Ext4_dir_ops 落盘 |
| 文件大小上限 | [✓] | 流式 offset I/O，无上限（QEMU 验证 6MiB 拷贝字节一致） |
| 并发写安全 | [✓] | 写穿透 + 服务器串行化无丢更新；O_APPEND 服务器端原子追加（`pappend` RPC：seek EOF+write 单 RPC 内完成，并发追加不互相覆盖；shell `>>` 已支持，tmpfs 同步支持） |
| cap 生命周期 | [✓] | op_release（客户端析构无条件调用）释放 Ext4_file_svr；子 namespace 路径去重缓存（2026-06-04） |
| tmpfs / ramfs | [✓] | 进程内 RAM /tmp（pkg/tmpfs，链入 native_shell；inode/handle 分离，open/读写/mkdir/unlink/readdir 全通） |
| /proc / /sys | [✓] | 合成只读 VFS（pkg/procfs，链入 native_shell，读时生成）：/proc/{uptime,meminfo,version,cpuinfo,tasks}、/sys/{cpu_online,version}（2026-06-09）|

**缺口 3 已完整闭合**（2026-06-09）。剩余皆为增量：tmpfs 后续（容量上限 / 跨进程共享 / 通用挂载到 l4re Vfs 构造）、其他块设备挂 ext4。
战略优先级见上方进度小结：**P0 调度崩溃已于 2026-06-09 修复**（地基裂缝补上）；当前敞着的结构性
缺口为 **2（管道）/ 4（Shell）/ 6（安全）**——这是接下来的主攻方向。

#### 缺口 4：Shell 功能残缺

| 功能 | 状态 |
|------|------|
| `\|` 管道 | ✗ |
| `<` 输入重定向 | ✗ |
| `$VAR` 环境变量 | ✗ |
| `if / while / for` 控制流 | ✗ |
| PATH 路径搜索 | ✗（命令必须在内置列表） |
| `&&` `\|\|` 逻辑连接 | ✗ |
| Tab 补全文件路径 | ✗（只补全命令名） |

当前 shell 更像一个命令分发器，不是真正的 POSIX shell。

#### 缺口 5：日志系统设计缺陷

原 libklog 是进程内库，多进程写竞争问题已通过独立 `syslogd` 守护进程解决（各进程经 L4Re IPC 发消息，syslogd 串行写文件，[✓]）。**仍待解**：

- **无日志轮转**：syslog.txt 无限增长，嵌入式环境存储有限（按大小轮转 + 保留 N 份）
- **flush 时机不可控**：lvgl-demo 靠 UI loop 计时，精度差

#### 缺口 6：安全模型缺失

- 密码硬编码在源码（`root/12345678`）
- 无用户数据库（无 `/etc/passwd` 等价物）
- `run` 命令将 native_shell 的**全部** cap（含 sigma0）转发给子任务
- 无基于 cap 的细粒度权限策略
- 所有进程等效于 root，无隔离

---

## 已知问题（仍需要长期测试复现）

> **★ P0 调度崩溃 — 2026-06-09 已修复并验证（根因＝陈旧标志，非重入）**。用 `tools/smp_crash_rate.sh`
> 量化发现**登录提示符击键风暴是 100% 确定性复现器**（且 `-smp 1` 单核也崩，推翻"SMP-only"旧定性），
> 进 JDB 抓栈 + 诊断打印坐实：`schedule_in_progress` 的属主是**被抢占切走的另一个上下文**（idle/kernel
> context 在自己的 preemption_point 里把 CPU 交给刚唤醒的线程、来不及清标志）→ 后续线程进 `schedule()`
> 撞见陈旧标志。修法（`kernel/src/kern/context.cpp` 两处）：入口处清掉属主≠current 的陈旧标志（属主==current
> 仍是真重入、保留断言）；属主清除点改为"仅当==this 才清"防误清。验证：login 风暴 smp1/smp2 各 20 轮
> **0/20 崩**（修前 60/60＝100%），ext4 + spawn 冒烟回归全绿。

| 优先级 | 问题 | 现象 | 根因与修复 |
|--------|------|------|------|
| ~~P0~~ ✅ | **任务调度概率崩溃（已修 2026-06-09）** | `context.cpp:758: !schedule_in_progress` 断言，CPU 进 JDB 死循环；登录击键风暴 100% 复现（单核亦然） | **陈旧标志**（非重入）：某上下文 `schedule()` 置 `schedule_in_progress` 后在 `preemption_point` 窗口被 IRQ 抢占切走、未清标志 → 别的线程进 `schedule()` 撞陈旧标志。修复：`Context::schedule()` 入口清属主≠`current()` 的陈旧标志 + 属主清除点条件化（`==this` 才清）。复现/验证：`tools/smp_crash_rate.sh` |

**P0 回归门禁（2026-06-04）**：`tools/ci_smp_smoke.sh` 将调度概率崩溃做成 CI 冒烟测试。
反复无显示启动 `smp-spawn-bench-ci` entry（spawnd 反复 create/destroy + 跨核 IPC 噪声，
专压 `preemption_point()` 窗口），按串口输出判定 PASS / CRASH / HANG / MISCFG，
多轮统计失败率，全过才 `exit 0`。用法：
```bash
ENTRY=smp-spawn-bench-ci ./build.sh --board virt bootstrap   # 构建 CI 镜像（一次）
tools/ci_smp_smoke.sh -n 20                                  # 跑 20 轮回归
```
两个 entry：`smp-spawn-bench`（`-D 30 -s 2`，交互/长压）与 **`smp-spawn-bench-ci`**
（`-D 8 -s 3`，CI 默认，单轮 ~10s，单位时间样本数约 3×；cfg 见
`conf/smp-spawn-bench-ci.cfg`）。判据要点（防误报）：bench 自身会打印
"kernel panic = fix regressed"，故**唯一**通过判据是 `PASS ... without kernel panic`
行；崩溃签名仅用 `schedule_in_progress` / JDB 进入横幅等专属 token。建议每次动
调度器 / spawnd / `Switch_lock` 后跑一轮，并定期 `-n 50+` 测长期崩溃率（概率缺陷，
单次通过不证明已修复）。

**⚠️ 门禁现状（2026-06-09）— 待迁移**：定位 P0 时用新工具 `tools/smp_crash_rate.sh`
（崩溃率矩阵测量器，扫 workload×smp 网格）量化发现：**spawn-bench 门禁现已基本看不见
这个 bug**——spawn smp2/smp4 各 100 轮 **0/200 全过**，而**登录提示符击键风暴** smp1/smp2
各 100 轮在修复前 **100% 必崩**（且 `-smp 1` 单核亦崩，推翻"SMP-only"旧定性）。即现有
`ci_smp_smoke.sh`（spawn）对这条已知 100% 复现的崩溃形同虚设。**待做**：把 `ci_smp_smoke.sh`
的工作负载换成/补上 login 击键风暴（复用 `smp_crash_rate.sh` 的 `feed_login` 手法），否则
门禁守不住此 P0 的回归。修复后全量 soak：login+spawn 各格 100 轮＝ **400/400 全过 0 崩**。

**P1 cd 概率卡死 — 已修复（2026-06-08）**：根因是 VFS 层 `meta_probe` / `Env_dir::check_type`
（`l4re/l4re_vfs/include/impl/ns_fs_impl.h`）对 initial_caps 发 Meta IPC 时，*发送*超时虽
已为 0（peer 未在 receive-wait 则快速失败），但*接收*侧仍用 `L4_IPC_TIMEOUT_NEVER`——
当 sigma0 恰好处于 receive-wait、收下消息却不回复时，接收永久阻塞，挂死 `cd` / Tab 补全。
修复：接收侧改为有限超时（50ms，远高于 cyclictest 实测 ~3ms 最坏往返，不会误判响应正常
的 namespace 服务器）。QEMU 验证：6× `cd /ext4` + `ls` + `cd /` 循环全部成功、shell 存活
标记打印、无崩溃无挂起。属概率缺陷，单次通过不构成 before/after 复现，但逻辑根因已消除。

---

## P1 — 核心系统服务

### 文件系统 / VFS

| 状态 | 子任务 |
|------|------|
| [✓] | VirtIO 块设备驱动（pkg/virtio-block-driver） |
| [✓] | block driver 崩溃根因修复：l4re 上游同步后 `l4re_env_t::caps` 偏移从 48 变 52，旧 binary 访问错字段（`first_free_utcb`）→ UTCB 越界；重编后消除 |
| [✓] | ext4fs 服务器挂载 + I/O 自测（lwext4） |
| [✓] | L4Re Namespace 服务器 — POSIX 只读访问（cat /ext4/file） |
| [✓] | 写支持 — `echo > /ext4/file`、`cat` 读回（共享 Dataspace + op_close 回写） |
| [✓] | 目录列举 — `ls /ext4` 及子目录递归（`.dirinfo` DS 机制） |
| [✓] | `mkdir` / `unlink` 支持（`mkdir` / `rm` 命令 → Ext4_dir_ops `ext_mkdir` / `ext_unlink`） |
| [✓] | cap 生命周期管理（2026-06-04）：每次 open 的 `Ext4_file_svr` 由客户端析构无条件发 `op_release` → 服务器 `unregister_obj` + 自删除（读写都回收，含从不调 op_close 的只读 `cat`）；子 `Ext4_namespace` 改为按路径去重缓存（无状态路由器，安全复用），把每次查询泄漏降为每个不同目录一份。QEMU 验证：8× release、写回完整性、6× 重复 cat 无崩溃、子目录 4× ls 缓存命中正常。同时修复 `op_close(0)` 会截断文件的潜在 footgun（改为跳过 flush）|
| [✓] | 流式 I/O 重设计（2026-06-08）：废弃整文件缓冲，改为 setup/pread/pwrite offset 模型 + 64KiB 弹跳缓冲；一举解决 4MiB 上限、并发写丢更新、每句柄内存。详见 [[缺口 3]] 表 |
| [✓] | 原子 O_APPEND（`pappend` RPC，服务器 seek-EOF+write 单 RPC 内完成）+ shell `>>` |
| [✓] | tmpfs（pkg/tmpfs，进程内 RAM /tmp，inode/handle 分离） |
| 待做 | 其他块设备（emmc-driver、nvme-driver）挂载 ext4 |
| 待做 | tmpfs 容量上限（防失控写耗尽 48MiB RAM）/ 跨进程共享 / 通用挂载到 l4re Vfs 构造 |

详细设计见 [ext4-implementation-plan.md](ext4-implementation-plan.md)。

### 网络栈

| 状态 | 子任务 |
|------|------|
| [✓] | lwIP 集成，virtio-net，TCP echo server |
| [✓] | `net` / `ifconfig` 命令集成到 native_shell |
| [✓] | UDP 支持验证（udp 命令，echo server port 5001，hostfwd=udp::5556-:5001） |
| [✓] | DNS 解析（nslookup 命令，lwip_getaddrinfo，DNS 服务器 10.0.2.3） |
| [✓] | ping 命令（ICMP raw socket，支持 -c 计数，RTT 统计） |
| [✓] | DHCP 客户端（2026-06-09）：lwIP DHCP，`dhcp [release]` 命令 + `IFCONFIG_IP4_vn0=dhcp` 启动时动态获取（失败回退静态）。QEMU 验证：从 SLIRP DHCP 获到 10.0.2.15/24 gw 10.0.2.2 DNS 10.0.2.3，release 正常。静态仍为默认（快启动/兼容） |
| 待做 | HTTP 客户端（wget / curl 最小实现） |

#### 拆解 native_shell ① 网络栈 → 独立 `netd`（socket-over-IPC，2026-06-11）

native_shell 把整套 lwIP + 手搓 virtio-net 驱动塞在进程内，还为此持 **sigma0**（裸映射全物理内存 = 安全缺口 6 最大洞）。Phase 1 把网络栈抽成独立 `pkg/netd` 服务器，立住"客户端经 IPC 用网络、不再碰 lwIP/sigma0"的架构地基。

| 状态 | 子任务 |
|------|------|
| [✓] | **netd Phase 1（2026-06-11）**：新 `pkg/netd` 服务器，**经 vbus（非 sigma0）独占 virtio-net 网卡** + 跑 lwIP/DHCP/静态 IP；协议 `Net_svr`（`L4::Kobject_t<…,0x5904>`：`tcp_connect`/`send`/`recv`/`close`，inline `Ipc::Array`）；内部用 lwIP BSD socket 实现 RPC。virtio 驱动/lwIP/DHCP 原样搬自 `cmd_net.cc`，唯一实质改动＝MMIO 映射走 vbus iomem（仿 virtio-block-driver）而非 `l4sigma0_map_iomem`（DMA 仍用 user_factory + Phys_space，与 sigma0 无关）。native_shell 加 `tcp <ip> <port> <msg>` 命令（`get_cap<Net_svr>("netd")` 客户端）。ned：`virt-full.io` 加 `vbus_net=SLOT0`；`native-shell.cfg` 起 netd 只给 `{vbus, svr}`（**无 sigma0**），native_shell **摘掉 sigma0 cap** 换 `netd` cap。**头号安全 win：sigma0 从 native_shell 彻底消失**。QEMU 端到端验证（`tools/netd_verify.py`，pexpect）：`tcp 10.0.2.2 5000 hello_netd_phase1` → netd `tcp_connect`/`sent 17`/`recv 17 bytes: hello_netd_phase1`（shell→IPC→netd→lwIP→网卡→主机 echo→回 全链路），netd 日志 `virtio-net at vbus MMIO`/`stack ready`；ci_smp_smoke 3/3 |
| 代价（明确） | Phase 1 让 netd 独占网卡，native_shell 进程内 lwIP 随之休眠（`net_auto_init` 无 sigma0 自禁用，`net_is_ready()`=false）：现有 8 个 net 命令（`net/ifconfig/dhcp/ping/nslookup/udp/mqtt/telnetd`）**暂时失效**，待后续阶段逐个迁到 netd 客户端路径后恢复 |
| [✓] | **拆解 native_shell ③ 首步（2026-06-11）：netcat 独立进程**——新 `pkg/netcat`（独立 task，**不链 lwIP、不持 sigma0**），网络全经 `Net_svr` IPC（`get_cap<Net_svr>("netd")` → tcp_connect/send/recv/close）。接线：`native-shell.cfg` 给 **spawnd** 加 `netd` cap（spawnd 按名转发全部 initial caps 给子进程，故 `run rom/netcat` 自动拿到 netd），同时把 netd cap 从 **shell_caps 移除**（shell 自身不再持任何网络 cap），并**删掉 native_shell 里临时的 `tcp` 命令**（真正瘦 shell）。`modules.list` +netcat。QEMU 验证（`tools/netd_verify.py` 改驱 `run rom/netcat 10.0.2.2 5000 hello_netd_phase1`）：spawnd `spawned: rom/netcat (task=428000)`、netd `tcp_connect -> handle 3`、netcat `sent 17`/`recv 17 bytes: hello_netd_phase1`（独立进程→IPC→netd→lwIP→网卡→主机 echo→回 全链路）；ci_smp_smoke 3/3。证明"net app 作隔离进程、网络仅经 netd IPC"模式 + spawnd cap 转发接线 |
| [✓] | **ifconfig 迁为独立工具（2026-06-11）**：netd 加 `ifconfig` 只读查询 RPC（op_ifconfig 把 n_netif 格式化成文本返回 `Ipc::Array<char>`）；新 `pkg/net-cluster/tools/ifconfig`（建立 net-cluster **tools/** 目录＝独立 net 工具，区别于 protocol/ 链入 shell 的库）——独立 task、不链 lwIP/不持 sigma0，`get_cap<Net_svr>("netd")->ifconfig` 查询并打印；经 spawnd 转发 netd cap，`run rom/ifconfig` 跑通。native_shell 删掉休眠的 `ifconfig` builtin（表项+decl+cmd_net.cc 里 cmd_ifconfig 函数体——即用户看到的 "No network interfaces. Run 'net' first." 源头）。验证 `tools/ifconfig_verify.py`：`vn0: flags=1043<UP,BROADCAST,RUNNING,MULTICAST> ... inet 10.0.2.15 ... ether 52:54:00:12:34:56`；ci_smp_smoke 3/3。确立"legacy net 命令逐个迁为 net-cluster/tools 下独立 netd 客户端程序"的模式 |
| [✓] | **dhcp/ping/nslookup 迁为独立工具（2026-06-11）**：netd 扩协议加 `resolve`(DNS→ip_be)、`ping_one`(单包 ICMP，返回 rtt_us+单行文本——**绕开 inline IPC ~250B 上限**：整段 ping 输出会撞 L4_EMSGTOOLONG/-1002，故拆成每包一次 RPC，工具侧循环/计时/汇总)、`dhcp`(action 0renew/1release/2status→文本)。新 `pkg/net-cluster/tools/{dhcp,ping,nslookup}`（独立 task、不链 lwIP/sigma0、经 spawnd 转发 netd cap）；ICMP 逻辑搬自 cmd_ping.cc。native_shell 删 dhcp/ping/nslookup builtin 表项（函数体暂留——cmd_ping.cc 还有未迁的 udp）。`netcat` 也归并到 tools/（从 pkg/netcat 迁入）。验证 `tools/nettools_verify.py`：nslookup 10.0.2.2→Address、ping -c2→64 bytes/rtt min/avg/max、dhcp→IP 10.0.2.15；ci_smp_smoke 3/3。坑：工具用 l4_sleep（uclibc usleep 需 feature macro）|
| [✓] | **netd Phase 2 起步 + net/udp 迁移（2026-06-12）**：echo 服务器是长驻/阻塞 socket，会卡死 netd 单线程 server.loop——解法＝**netd 每服务起一个 detached worker pthread**（lwIP 线程安全），由 `udp_echo(port)`/`tcp_echo(port)` RPC 控制启动，server.loop 保持响应。worker 线程跑整个 bind/accept/echo 循环（逻辑搬自 cmd_udp/net_server_thread）。新 `pkg/net-cluster/tools/{udpecho,tcpecho}` 瘦控制工具（`run rom/{udpecho,tcpecho} [port]`→netd 启服务）。native_shell 删 net/udp builtin。验证 `tools/echo_verify.py`（主机经 hostfwd tcp::5555/udp::5556 连 guest echo，回 hello_tcp/hello_udp）PASS；ci_smp_smoke 3/3 |
| 待做 | **telnetd + mqtt 迁移（剩 2 个，较难，迁完才能摘 lwIP）**：telnetd＝远程 shell，需 netd accept + 把 socket 接到 spawn 的 shell I/O（console-over-IPC，难）；mqtt＝客户端，MQTT 框架可搬到工具用 netd tcp_connect/send/recv，但 TLS(mqtts) 需 netd 侧 TLS 或工具内 mbedTLS-over-netd（难）。两者各是独立子工程 |
| 待做 | netd Phase 2 续：listen/accept/UDP 的 socket-over-IPC（非内置 echo）、DNS-via-poll、**共享 DS 大数据**（绕 inline IPC ~250B 上限）、透明 libc socket 后端（`libc_be_socket_netd`）、终态删 cmd_net.cc/cmd_ping.cc/cmd_telnet.cc + 从 native_shell REQUIRES 摘 lwip/libc_be_socket_lwip/libsigma0/nc_mqtt/nc_telnet |
| [✓] | **拆解 native_shell ②（2026-06-11）：login/auth → 独立 authd + 文件 + 加盐哈希**——新 `pkg/authd`（`Auth_svr` 0x5905：`authenticate(user,pass)`→L4_EOK/-EPERM），凭据存 **/ext4/etc/shadow**（`user:salt_hex:hash_hex`，hash=SHA-256(salt‖pass)，复用 mbedTLS `sha256_*_ret` 流式 API；常量时比较）。authd 经 ext4 VFS（链 `ext4_client` + `-u ext4_client_module_init` + 持 `ext4` cap，`fopen("/ext4/etc/shadow")`，L4Re VFS 把命名 namespace cap 自动当目录解析）读凭据；无文件则退回内建 fallback（同哈希方案，明文不出现，仅防无盘 brick）。native_shell `do_login` 改 IPC 客户端（`authd->authenticate`），**删 LOGIN_USER/LOGIN_PASS 硬编码**（shell 二进制零密码），fail-closed（无 authd 拒登）。ned 起 authd 只给 `{svr, ext4}`（无 console/网络），shell 拿 `authd` cap。`tools/seed_auth.sh`（debugfs，须单会话 `cd /etc`+相对名 write，否则泄漏 inode 不链接）种盘。QEMU 验证（`tools/auth_verify.py`）：authd `loaded 1 credential from /ext4/etc/shadow`、错密码 `auth FAIL`→`Login incorrect`、对密码 `auth OK`→`Welcome, root!`；ci_smp_smoke 3/3。直面安全缺口 6 的"密码硬编码"|
| 待做 | authd 续：passwd/useradd 工具、多用户、会话 token/uid、登录失败锁定（lockout）policy 移入 authd、shadow 文件权限/加密 |
| [✓] | **根目录 ls 整理（2026-06-11）：去重 dev + 服务/设备 cap 归类**——拆出 netd/authd 等服务后，`ls /` 里冒出一堆裸 cap（authd/fb/input/rtc/spawnd/syslogd）+ 两个 dev（驱动注册表 namespace cap "dev" 与 devfs 挂载点 /dev 同名）。改 l4re_vfs `Env_dir::getdents`（fork 内核心 VFS）：①根只列 **Namespace** cap（可浏览目录），裸服务/设备网关 cap 一律不列（仍可 get_cap 用，不影响按名 open）；②跳过与挂载点同名的 cap（dev 去重）。坑：vbus cap 也应答 Dataspace 协议（故 ns\|\|ds 过滤会漏 input），只列 Namespace 才干净；ldso 链最小 libc 无 strcmp，自写 env_dir_str_eq。服务 cap 归 **/svc**（procfs.cc 加 Gen_dir：authd/spawnd/syslogd，读时报协议+up/absent）；设备 cap 归 **/dev**（devfs 加 fb0/input0 stub 节点，按 cap 在场注册；rtc0 已有）。结果 `ls /`=dev/ ext4/ proc/ rom/ svc/ sys/ tmp/；`ls /svc`=authd spawnd syslogd；`ls /dev`=input0 null rtc0 zero。验证 `tools/ls_verify.py`；ci_smp_smoke 3/3 |

#### net-cluster — 对外连接套件（`pkg/net-cluster`，2026-06-09）

协议库链入 native_shell（lwIP 跑在 shell 进程内，故 mqtt/telnet 必须同进程）；并掉了空壳 `pkg/net-tools`。

| 状态 | 子任务 |
|------|------|
| [✓] | MQTT 客户端（`libnc_mqtt`，`mqtt pub\|sub <broker> <topic> [msg\|secs]`）：编入 lwIP `apps/mqtt`，明文 QoS0，连接/发布回调跑在 tcpip 线程→命令线程轮询标志；所有 `mqtt_*` 调用持 `LOCK_TCPIP_CORE`。QEMU 验证：向主机 Python broker stub `PUBLISH turingos/test hello_from_turingos` 成功 |
| [✓] | telnet 服务（`libnc_telnet` + `cmd_telnet.cc`，`telnetd [port]`）：单前台会话，`dup2(sock, STDOUT)` 复用全部 shell 命令，剥离 IAC，行模式分发。QEMU 验证：主机 telnet 客户端经 hostfwd 远程跑 `uname`/`uptime`/`exit` 回显正常 |
| [✓] | **TLS/crypto 后端（mbedTLS 2.28 → lwIP altcp_tls，2026-06-10）**：移植 mbedTLS 2.28.10 LTS 为 L4Re 库 `pkg/mbedtls`（vendored `library/*.c`，剔除 net_sockets；`mbedtls_l4_config.h` 关 FS_IO/NET_C/TIMING_C/PSA、开 ENTROPY_HARDWARE_ALT）；lwIP 开 `LWIP_ALTCP_TLS_MBEDTLS` + 编入 altcp_tls glue；mqtt 加 `pubs`/`subs`（TLS :8883，no-verify 首版）。QEMU 验证：向主机 TLS broker 完成 `TLSv1.2 ECDHE-RSA-AES256-GCM-SHA384` 握手 + `PUBLISH turingos/test hello_tls`，guest `connected (TLS)`/`published`，无崩溃 |
| 待做 | **真熵源**（首版熵＝弱软件 jitter，`pkg/mbedtls/lib/entropy_l4.c`，标注勿用于生产）：可插拔后端，按板补 virt→virtio-rng 客户端驱动 / imx6ul→RNGB 硬件 TRNG / BBB→jitter+ADC |
| 待做 | TLS **证书校验**（首版 no-verify，CA=NULL；需 CA 供给）；mbedTLS 头文件正式 export（现 net-cluster 用 PRIVATE_INCDIR 直连）；**ssh**（有 TLS 后可上）；HTTP/https 客户端 |
| 待做 | telnetd 并发多会话（需解决 STDOUT 多路复用） |

### 串口通信

| 状态 | 子任务 |
|------|------|
| [✓] | virtio-serial Vcon 服务器 + uart-test 客户端（QEMU virtio-serial 验证通过） |

---

## P2 — 功能扩展

### 进程管理与 IPC

| 状态 | 子任务 |
|------|------|
| [✓] | spawnd 程序加载服务：独立进程，接受 spawn/wait/kill IPC，从 ROM 加载 ELF 并管理子任务生命周期 |
| [✓] | `run` 命令接入 spawnd：shell 通过 IPC 调用 spawnd，不再内嵌 libloader |
| [✓] | spawnd Phase 2：`jobs`/`wait`/`kill` 命令支持（g_jobs 表，handle 持久化跨 IPC 调用） |
| [✓] | spawnd Phase 3：从 ext4 加载 ELF（`run /ext4/bin/foo`，open_from_ext4 via Env_ns） |
| 待做 | 管道（pipe）：基于共享 DS + 环形索引实现字节流，支持 `cmd1 \| cmd2` |

### Shell 增强

| 状态 | 子任务 |
|------|------|
| [✓] | `&` 后台、`>` 重定向、`>>` 追加重定向（2026-06-08，配合 ext4/tmpfs O_APPEND）、Tab 命令名补全、历史记录 |
| 待做 | `\|` 管道（依赖上方 pipe IPC） |
| 待做 | 环境变量（`export VAR=val`、`$VAR` 展开） |
| 待做 | PATH 路径搜索（从 `/ext4/bin` 等目录查找可执行文件） |
| 待做 | Tab 补全文件路径 |
| 待做 | `<` 输入重定向 |
| 待做 | `if / while / for` 基本控制流（mini shell 脚本） |

### 日志系统演进

| 状态 | 子任务 |
|------|------|
| [✓] | libklog：进程内 ring buffer，ANSI 彩色控制台，ext4 追加写入 |
| [✓] | syslogd 守护进程：统一接收各进程 IPC 日志消息，串行写文件，消除并发竞争（`Klog_send_nowait` 零超时 IPC 防死锁） |
| 待做 | 日志轮转：按大小（如 1 MiB）轮转，保留最近 N 个文件 |
| 待做 | `dmesg` 命令优化：支持 `-f facility` 过滤、`-l level` 过滤、`-w` follow 模式 |

### 显示子系统

| 状态 | 子任务 |
|------|------|
| [✓] | fb-test：帧缓冲测试应用，QEMU ramfb 验证通过 |
| [✓] | fb-drv 第一阶段：用户态 Goos 代理服务器，client 通过 IPC 访问帧缓冲 |
| [✓] | LVGL v9 图形演示（lvgl-demo，QEMU virtio-gpu，流畅渲染） |
| [✓] | virtio-input：键盘 + tablet 指针接入 LVGL（keypad + pointer indev） |
| [✓] | 从 native_shell `run` 命令启动 lvgl-demo（`run lvgl-demo`，自动转发 fb/input cap） — 已修复 Moe 外部 RM 兼容性问题（见下注） |
| 待做 | fb-drv 第二阶段：多客户端 virtual buffer + 合成（轻量窗口管理器基础） |
| 待做 | fb-drv 第三阶段：RPi4 HDMI 真实硬件路径（BCM2711 mailbox） |

**注 — Moe 外部 RM 兼容性（2026-05-07 修复）**：通过 `run` 命令启动的任务使用 Moe 外部 RM，ned 直接启动的任务使用 ITAS 内部 RM。Moe RM 的 `validate_ds()` 只接受自己 `object_pool` 里的 Dataspace，io-server vbus cap 不在其中，导致 `rm()->attach()` 返回 `-L4_ENOENT`。修复方案：
- **输入 MMIO**：改用 `l4sigma0_map_iomem()` 恒等映射（phys == virt），绕过 RM；保留 vbus-as-DS 作 ned 路径的 fallback。
- **帧缓冲**：将 `attach_buffer()`（`L4_SUPERPAGESHIFT`）改为直接 `rm()->attach()` + `L4_PAGESHIFT`，加入调试输出。
- **崩溃防护**：加 `lv_port_disp_is_ready()` 检查，display 失败时提前退出，避免 `lv_display_get_default()` 返回 NULL 导致页错误。

详细设计见 [fb-drv-design.md](fb-drv-design.md)。

**注 — libklog 系统级日志（2026-05-07~08）**：将原 native_shell 内部 `log.cc` 提取为独立库 `pkg/klog`，供所有包使用。特性：
- ANSI 彩色终端输出（红=ERR+，黄=WARN，暗灰=DEBUG）
- ext4 追加写入：`/ext4/syslog.txt`，单调序列号确保每次 flush 只写新条目
- 无 ext4 时静默降级为仅控制台输出
- 已接入：native_shell（log.h 改为 shim）、lvgl-demo（lv_port_disp/input/main）、cmd_net、cmd_ping
- 已修复：fopen("a") 在 Ext4_file_vfs 中 _pos=0 导致覆写问题（补 fseek SEEK_END）

### WebAssembly 运行时（wamr）[✓]

移植已完成，QEMU 验证通过：`add(40, 2) = 42`。
修复：`wasm_runtime_load` 会就地修改 buffer，传 const 数组会 segfault，需先 malloc 拷贝。

### Shell 任务管理 [✓]

- `&` 后台运算符，`list_tasks` 查看后台任务
- `run` 命令：从 ROM 动态启动已加载的 L4Re 程序（`run hello` / `run lvgl-demo`）
- `push_initial_caps` / `map_initial_caps`：将 native_shell 的所有命名 cap（fb、input、sigma0、rtc 等）透传给子任务，使子任务无需 ned 即可访问硬件服务
- **注（2026-05-07）**：`module hello` 曾在添加 fb-drv 时意外从 native-shell entry 中删除，导致 `run hello` 报 `open_file: 'hello' not found`，已在 modules.list 中补回

### 驱动框架（微内核设备模型）

把驱动从"库静态链进 native_shell 的巨石"重构成 **C 驱动服务器 / A 设备管理器 / B 注册表+自省** 三层（不照搬 Linux 内核态驱动核；微内核里驱动=用户态服务器+IPC+cap）。

| 状态 | 子任务 |
|------|------|
| [✓] | **Phase 0（2026-06-10）：ds18b20 抽成独立 `ds18b20-server`**——协议头 `Temp_svr`（`L4::Kobject_t<…,0x5902>` + `read_temp_c100` RPC）、`Epiface` 服务器（`Registry_server`，仿 spawnd）、ned 只给 `{svr, vbus}`（隔离，对比 shell 全量 cap）、native_shell `temp` 改 IPC 客户端、去 ds18b20 lib 链接。QEMU 验证：`temp` 读出活值 `25.xx °C (sim)`（真 IPC 往返）、驱动独立 task、无回归。砍了安全缺口 6 一角 |
| [✓] | **Phase 1（2026-06-10）：radio(tef6686hn) 迁成独立 `tef6686hn-server`**——`Radio_svr` 协议（0x5903，init/tune/seek/status/mute/volume）、有状态服务器（**调谐状态存在服务器、跨命令保持**：tune 101.1→status 显 101.1→seek→101.3→status 显 101.3）、多波段 FM/MW/LW、ned 只给 `{svr}`（无 i2c-server→sim）。验证：QEMU radio 全流程 IPC + 状态持久 + 独立 task；ci_smp_smoke 3/3。（rds 留作后续——结构体 marshaling） |
| 待做 | 把 Temp_svr/Radio_svr 提炼成**通用 class 协议**（独立 header 包，不再借 ds18b20/tef6686hn include）；rds RPC |
| [✓] | **Phase 2（2026-06-10）：dev Namespace 注册表**——复用 L4Re::Namespace（Moe，零新服务器）；ned `l:create_namespace({})` 建空可写 ns，驱动用无名 `register_obj(&impl)` 自分配网关 + `dev->register_obj("temp0"/"radio0", gate)` 自注册，native_shell `dev->query(name)` 按名解析（缓存）。ned 删 per-driver 通道与 shell 的 temp/radio cap，只给一个 `dev:m("r")`（只读）；驱动给 `dev:m("rws")`（坑：必须 :m() 显式 rights，裸 cap 不映射）。验证：启动日志 `registered as dev/temp0/radio0`，temp/radio 经注册表解析照常（状态持久）；ci_smp_smoke 3/3 |
| [✓] | **Phase 3（2026-06-10）：device-manager 动态启动**——新增 `pkg/devmgr`：按设备清单经一个**专用受限 spawnd 实例 `drvd`**（只配 `{dev:rws, vbus}`，spawnd 转发自身 initial caps→驱动只继承这套最小 cap，无 shell 的 sigma0/ext4）动态 spawn 驱动；ned 删掉逐个 `l:start` 驱动，改为起 drvd + devmgr。驱动经 drvd 起来后自注册进 dev（Phase 2 不变）。验证：`[devmgr] launching rom/ds18b20-server…2 launched`，驱动在 `drvd|` 日志下 `registered as dev/temp0/radio0`，temp/radio 照常+状态持久；ci_smp_smoke 3/3。（真·vbus 枚举 probe-gating / hotplug 留后续——devmgr 已是落点） |
| [✓] | **Phase 4（2026-06-10）：/sys/devices 自省**——procfs 加 `Devices_dir`/`Dev_file`（挂为 /sys 的 "devices" 子目录），`ls /sys/devices`→`temp0 radio0`、`cat /sys/devices/temp0`→name/class/**status:online**(查 dev 注册表的**活状态**)/driver。设备集来自共享 `device_table.h`（与 devmgr 同源）。坑：①子目录不能用嵌套 mount（被父 mount 遮蔽），要让 /sys 的 Gen_dir 自己返回子目录；②VFS 把整条剩余子路径 `devices/temp0` 传给 mount 的 get_entry，要 delegate 给子目录；③procfs 是 lib，改后 native_shell 须 `--pkg native_shell --force` 重链（否则旧 procfs 仍在）。验证：ls/cat 通、status 活、/proc 不受影响、ci_smp_smoke 3/3。**驱动框架 C→A→B 四阶段闭环** |
| 待做 | hotplug / 每设备多属性子目录全树 / 真·vbus 枚举 probe-gating / 通用 class 协议头 + rds |

### 传感器 / 外设驱动

| 状态 | 子任务 |
|------|------|
| [✓] | DS18B20 温度传感器（`temp` 命令 → ds18b20-server IPC，见上「驱动框架 Phase 0」；1-Wire/GPIO） |
| [✓] | TEF6686HN FM 收音机（`radio` 命令，I2C） |
| [✓] | MCP2515 CAN 控制器（SPI） |
| [✓] | AT24C02 EEPROM（I2C） |
| [✓] | `top` 命令：termbox2 TUI，800ms 双快照 CPU%，uptime，内存统计，颜色标注（spawnd task_stat IPC + l4_thread_stats_time） |

### 多核任务调度 [✓]

QEMU virt（-smp 2）SMP 验证通过：CPU1 线程由 L4Re scheduler affinity 固定，
完成 10 万次计数后原子信号 CPU0，输出 PASSED。

### 调度延迟基准测试 cyclictest [进行中]

测量 `l4_ipc_sleep_us()` 唤醒延迟（KIP clock 前后读差）。
详细差距分析与完整测试体系规划见 [benchmark-plan.md](benchmark-plan.md)。

| 状态 | 功能 |
|------|------|
| [✓] | 基础 sleep→wakeup 延迟测量 |
| [✓] | min / avg / max（per-thread + 汇总） |
| [✓] | **纳秒精度**（`l4_kip_clock_ns()`，输出 "X.XXX us"）（2026-05-15） |
| [✓] | **百分位统计**（p50/p90/p99/p99.9，直方图后处理）（2026-05-15） |
| [✓] | **标准差**（integer Newton sqrt，无 libm 依赖）（2026-05-15） |
| [✓] | **1000 桶直方图**（1µs/桶，overflow ≥999µs；原 100 桶扩展）（2026-05-15） |
| [✓] | 多线程（pthread，最多 8 个） |
| [✓] | 参数解析（-i / -l / -t / -p / -b） |
| [✓] | QEMU 双核 8 线程验证通过（ALL: min=36.288 us avg=976.607 us max=3099.136 us stddev=165.004 us） |
| [✓] | 运行时可配置：`run cyclictest -t 2 -l 300 -i 500` 直接生效（2026-05-15 修复 spawnd UTCB 覆写 + readline 崩溃） |
| 待做 | CPU 亲和性（`-a` 绑核，消除过度订阅干扰） |
| 待做 | Duration 模式（`-D` 秒，替代固定循环次数） |
| 待做 | 线程间距（`-d N`，各线程 interval 错开） |
| 待做 | 预热阶段（`-W N`，消除首次 cache miss） |
| 待做 | JSON / 文件输出（CI 回归对比） |
| 待做 | 裸机（BBB / RPi4）验证（QEMU 结果不具绝对参考意义） |

---

## P3 — 长期目标

### 服务监督（init 演进）

ned 目前承担 init 角色，但缺少：
- 服务崩溃自动重启
- 服务间依赖声明与健康检查
- 运行时动态添加/删除服务

方向：在 ned 之上构建轻量 supervisor（类 s6 / runit 风格），或直接扩展 ned。

### POSIX 兼容层

参考 QNX 设计，在 L4Re 之上提供 POSIX 接口（进程、信号、文件描述符）。
涉及：驱动框架统一、libc 集成、syscall 适配。

### 安全模型

- 用户数据库：实现 `/ext4/etc/passwd`，支持多用户登录
- cap 权限策略：`run` 命令按需传递 cap 而非全量转发（尤其不应转发 sigma0）
- 进程隔离：不同权限进程访问不同 cap 集合

### 终端登录 [✓]

串口登录已实现：`login:` 用户名回显，`Password:` 星号掩码，3 次失败后重试。
默认账号 root / 12345678。

### 功耗与电源管理

`pkg/acpica` 已集成 ACPI 解释器。
- 短期：了解 ACPICA 初始化流程，能读取 ACPI 表
- 注意：BBB / RPi4 无标准 ACPI，该功能主要面向 x86 平台

### BBB（BeagleBone Black）硬件验证

AM335x 平台适配代码已完成（时钟初始化、UART、I2C、RTC、GPIO）。
开发板不在身边，待上电验证。详见 [beaglebone_black.md](beaglebone_black.md)。

---

## 参考：构建系统规则

添加新包 `pkg/<name>/` 的正确步骤：

1. 创建 `pkg/<name>/Control`，列出所有 `REQUIRES_LIBS` 依赖
2. Makefile 的 `PKGDIR` 层级：顶层用 `.`，`server/` 用 `..`，`server/src/` 用 `../..`
3. 编译：`make -C build/l4re_virt pkg/<name>`（不要用 `PKGS=` 变量）
4. `build.sh` 会自动同步 `l4mk/pkg/` 符号链接，无需手动 `ln -s`
5. 运行镜像：在 `l4mk/conf/modules.list` 添加 entry，用 `E=<entry> elfimage` 打包
6. **重要**：修改任何包后必须执行 `make -C build/l4re_virt E=<entry> elfimage` 重新打包引导镜像，`pkg/bootstrap` 不会自动检测二进制变化

---

## 构建系统增量编译可靠性 [✓]

**问题**：`pkg/` 下的包有两套构建输出目录，`make -C build/l4re_virt pkg/<name>` 只触发
`build/l4re_virt/pkg/<name>/`，不触发 `build/l4re_virt/ext-pkg/.../<name>/`。
后者才是实际打入镜像的二进制。修改头文件后，`make` 误认为 binary 已是最新，改动不生效。

**根因**：L4Re make 系统将 `l4mk/pkg/` 下的符号链接包（指向 `TuringOS/pkg/`）视为
"external package"，产出放入 `ext-pkg/<绝对路径>/` 而不是 `pkg/`。直接 `make pkg/<name>`
只构建 `pkg/` 目录（stale/older build），不构建 `ext-pkg/`。

**解决（2026-06-08）**：`build.sh` 新增 `--pkg <name>` 选项，自动在 `pkg/` 与 `ext-pkg/`
两处定位该包所有 `OBJ-*` 目录（库 client/lib 先于可执行 server，保证 `--force` 下重链顺序）
逐个编译，再自动重打包引导镜像，替代手动 find + bootstrap：
```bash
./build.sh --board virt --pkg native_shell          # 重建并重打包
./build.sh --board virt --pkg ext4 --force           # clean 重建（依赖库变动后强制重链）
ENTRY=cyclictest ./build.sh --board virt --pkg native_shell   # 指定重打包入口
```
`--force`（clean+重建）也解决"改了 l4re 核心头文件（libc/l4re_vfs/ldso）后，依赖它的包
二进制不自动重链"的问题（本会话曾两次踩到）。
