# TuringOS 待办事项

按优先级排列。



## P0 — 基础可用性

### 用户态 Shell


## P1 — 核心系统服务

### 文件系统服务

有 emmc-driver 和 nvme-driver 块设备驱动，但没有文件系统层。需要一个 VFS server 将块设备暴露为文件接口，否则存储驱动读出的数据无人消费。
备注：我看有ROMFS,看看这个怎么使用起来。

### 网络栈

有 virtio-net 驱动和 virtio-net-switch，
lwip协议栈已经有了，看看接下来还有哪些其他的需要优化的。

## P2 — 功能扩展

### 更多启动项配置

`conf/modules.list` 当前只有 `fiasco-base-test` 一个 entry。需要为不同驱动组合添加独立的启动项（如 emmc 测试、uvmm 虚拟机等），方便开发和测试。

### 设备树支持

有 libfdt 库但没有 `.dts`/`.dtb` 文件或构建流程。RPi4 和 BBB 都依赖设备树，当前 bootstrap 使用硬编码平台配置。要支持更多外设需要打通 DT 编译和加载流程。


### 参考qnx的代码

实现一个驱动框架；POSIX兼容；Shell；

## P3 - 当前需要改进的内容

1. 测试多核心任务调度；
2. 设备驱动 ftd，tree，VFS挂载
3. net tools，ifconfig和ping 
4. wamr
5. shell里启动程序