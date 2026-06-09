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
> 另有跨越所有方向的 **P0 调度概率崩溃**——目前只缓解 + CI 门禁守着，根因（Fiasco 已知设计缺陷）未除，
> 是任何实时/工业方向的硬闸门。

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
战略优先级见上方进度小结：缺口 **2（管道）/ 4（Shell）/ 6（安全）** + 已推后的 **P0 调度崩溃**。

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

> **★ 指定的下一个 P0（用户 2026-06-08 主动推后，"先记下，稍后解决"）**：下方调度概率崩溃是
> 当前唯一 P0、整个系统的地基裂缝、RT/工业方向的硬闸门。目前只**缓解 + CI 门禁守着，根因未除**。
> 攻它的打法：用 `tools/ci_smp_smoke.sh -n 100` 量化 baseline 崩溃率 → 逐路径把 SMP 调度里直接调
> `schedule()`（而非 `schedule_if()`）的地方收掉 → 每改一版重测前后崩溃率（概率缺陷，单次过不算修）。

| 优先级 | 问题 | 现象 | 怀疑点 |
|--------|------|------|------|
| P0 | **任务调度概率崩溃** | SMP=2 下偶发 `context.cpp:758: !schedule_in_progress` 断言，CPU 进入 JDB 死循环 | `Context::schedule()` 在 `preemption_point()`（sti→cli 窗口）开中断时，硬件 IRQ 触发的调度路径（`switch_to_locked` / `Switch_lock::help`）直接调 `schedule()` 而非 `schedule_if()`，重入断言；已初步修复两处调用点，但其他路径可能仍存在同类问题 |

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

### 传感器 / 外设驱动

| 状态 | 子任务 |
|------|------|
| [✓] | DS18B20 温度传感器（`temp` 命令，GPIO 1-Wire） |
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
