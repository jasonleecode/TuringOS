# TuringOS 待办事项

按优先级排列。

## P0 — 基础可用性

### 用户态 Shell

`pkg/apps/` 当前为空，`pkg/servers/` 只有空的 include 目录。drivers 层已具备 emmc、i2c、nvme、virtio-net 等驱动，但缺少可交互的用户态程序。`conf/modules.list` 注释中提到的 `native_shell` 尚未实现。

目标：实现一个串口交互 shell，作为第一个真正的用户态应用，加入 modules.list 由 ned 拉起。

### CI 自动化

当前没有 CI 配置。`build.sh` 和 `run_qemu.sh --check` 已具备自动化基础，需要加 GitHub Actions 做：

- 交叉编译（RPi4 + BBB）
- QEMU smoke test（`run_qemu.sh --check`）
- 子模块更新后的构建回归检测

## P1 — 核心系统服务

### 文件系统服务

有 emmc-driver 和 nvme-driver 块设备驱动，但没有文件系统层。需要一个 VFS server 将块设备暴露为文件接口，否则存储驱动读出的数据无人消费。

### 网络栈

有 virtio-net 驱动和 virtio-net-switch，但缺少 TCP/IP 协议栈。需要集成 lwIP 或类似的网络服务进程。

## P2 — 功能扩展

### 更多启动项配置

`conf/modules.list` 当前只有 `fiasco-base-test` 一个 entry。需要为不同驱动组合添加独立的启动项（如 emmc 测试、uvmm 虚拟机等），方便开发和测试。

### 设备树支持

有 libfdt 库但没有 `.dts`/`.dtb` 文件或构建流程。RPi4 和 BBB 都依赖设备树，当前 bootstrap 使用硬编码平台配置。要支持更多外设需要打通 DT 编译和加载流程。

### Wasm 运行时集成

`pkg/wasm/` 目前为空。需要选型（wasm3 / wamr / wasmtime）并集成到构建系统和 Kconfig。


### 参考qnx的代码

实现一个驱动框架；POSIX兼容；Shell；

## P3 - 当前需要改进的内容

