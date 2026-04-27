# TuringOS 待办事项

> 按优先级排列，`[✓]` 表示已完成。

---

## P0 — 核心基础

| 状态 | 任务 |
|------|------|
| [✓] | 用户态 Shell（native_shell，含 readline、历史记录） |
| [✓] | 构建系统包发现机制（Control 文件、PKGDIR 层级、sync_pkg_symlinks） |
| [✓] | 多启动项配置（modules.list 已有 native-shell / tcp-server / fb-test / fb-drv） |
| [✓] | TCP echo server（lwip + virtio-net，QEMU NAT 转发验证通过） |
| [✓] | SMP 双核默认（run_qemu_virt.sh -smp 2，smp-test PASSED） |

---

## P0.5 — 上游同步（阻塞性）

### 同步 l4re / kernel fork 分支

fork 的 l4re 和 kernel（Fiasco）分支已落后于上游主分支，需要 merge 后才能继续追踪新功能和安全修复。

- [ ] 确认当前 fork 落后的 commit 数量（`git log HEAD..upstream/main`）
- [ ] merge 上游 l4re-core 变更，解决冲突，验证构建不回归
- [ ] merge 上游 fiasco kernel 变更，验证 QEMU virt / BBB 启动正常
- [ ] 更新 submodule 引用，提交到 TuringOS 主仓库

---

## P1 — 核心系统服务

### 文件系统 / VFS

| 状态 | 子任务 |
|------|------|
| [✓] | VirtIO 块设备驱动（pkg/virtio-block-driver） |
| [✓] | ext4fs 服务器挂载 + I/O 自测（lwext4） |
| [✓] | L4Re Namespace 服务器 — POSIX 只读访问（cat /ext4/file） |
| [✓] | 写支持 — `echo > /ext4/file`、`cat` 读回（共享 Dataspace + op_close 回写） |
| [✓] | 目录列举 — `ls /ext4` 及子目录递归（`.dirinfo` DS 机制） |
| 待做 | cap 生命周期管理：`Ext4_file_svr` / 子 namespace 注册后不自动释放，长期运行会耗尽 cap slot |
| 待做 | 写并发：同一文件多个 op_query 产生多个 svr，末次 op_close 覆盖前面写入 |
| 待做 | `mkdir` / `unlink` 支持 |
| 待做 | 其他块设备（emmc-driver、nvme-driver）挂载 ext4 |

详细设计见 [ext4-implementation-plan.md](ext4-implementation-plan.md)。

### 网络栈完善

lwip 协议栈已集成，TCP echo server 可用。待完成：
- `ifconfig` / `ping` 等 net tools 集成到 native_shell
- UDP 支持验证
- DNS 解析

### 日志系统

当前各组件直接用 `printf` 输出，缺乏统一日志级别和持久化。
- 目标：实现简单的日志服务（severity 过滤、时间戳、可选写入存储）

---

## P2 — 功能扩展

### 显示子系统

| 状态 | 子任务 |
|------|------|
| [✓] | fb-test：帧缓冲测试应用，QEMU ramfb 验证通过 |
| [✓] | fb-drv 第一阶段：用户态 Goos 代理服务器，client 通过 IPC 访问帧缓冲 |
| 待做 | fb-drv 第二阶段：多客户端 virtual buffer + 合成（轻量窗口管理器基础） |
| 待做 | fb-drv 第三阶段：RPi4 HDMI 真实硬件路径（BCM2711 mailbox） |
| 待做 | 简单 GUI 框架（基于 fb-drv 第二阶段） |

详细设计见 [fb-drv-design.md](fb-drv-design.md)。

### wamr（WebAssembly 运行时）[✓]

移植已完成，QEMU 验证通过：`add(40, 2) = 42`。
修复：`wasm_runtime_load` 会就地修改 buffer，传 const 数组会 segfault，需先 malloc 拷贝。

### Shell 里启动程序

目前 native_shell 只能运行内置命令，不能 fork/exec 其他 L4Re 任务。
- 目标：通过 ned 或类似机制，在 shell 中动态启动已加载到 ROM 的程序

### 设备树支持

有 libfdt 库，但无 .dts/.dtb 文件和构建流程。RPi4 / BBB 依赖设备树。
- 目标：补充 dts 文件、打通 dtc 编译流程、bootstrap 加载 DTB

### 多核心任务调度 [✓]

QEMU virt（-smp 2）SMP 验证通过：CPU1 线程由 L4Re scheduler affinity 固定，
完成 10 万次计数后原子信号 CPU0，输出 PASSED。
run_qemu_virt.sh 默认已改为双核启动，支持 `--cfg smp-test` 选择镜像。

---

## P3 — 长期目标

### POSIX 兼容层

参考 QNX 设计，在 L4Re 之上提供 POSIX 接口（进程、信号、文件描述符）。
涉及：驱动框架统一、libc 集成、syscall 适配。

### 终端登录

用户名 / 密码认证，串口或 VNC 终端登录界面。

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
