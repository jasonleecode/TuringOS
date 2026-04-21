# Ext4文件系统实现计划

## 概述
本文档描述了如何为TuringOS的QEMU virt平台实现完整的ext4文件系统支持，利用已配置的100MB虚拟磁盘存储。

## 当前状态分析

### ✅ 已完成功能
- **QEMU虚拟磁盘**: `run_qemu_virt.sh`已配置100MB VirtIO块设备
- **block设备库**: `libblock-device`库存在，可处理块设备请求
- **VirtIO接口**: `l4virtio`库提供VirtIO设备支持
- **ext4库**: `pkg/filesystem/ext4/contrib/`包含完整的lwext4实现
- **ext4服务器结构**: `pkg/filesystem/ext4/server/`存在但只有stub实现

### ❌ 缺失功能
- **VirtIO块设备驱动**: 没有类似`ahci-driver`的VirtIO块设备驱动
- **ext4服务器实现**: 仅有接口stub，缺少实际文件系统操作
- **磁盘格式化工具**: 没有创建ext4文件系统的工具
- **系统启动集成**: 没有启动配置挂载文件系统
- **native_shell集成**: shell无法访问文件系统

## 需要实现的功能模块

### 1. VirtIO块设备驱动 (`pkg/virtio-block-driver/`)

**目的**: 初始化和操作QEMU提供的VirtIO块设备

**需要实现**:
```c
// pkg/virtio-block-driver/server/src/main.cc
- VirtIO设备初始化
- 块设备能力注册
- 与libblock-device集成
- 处理I/O请求队列

// 参考实现:
- pkg/ahci-driver/server/src/main.cc
- pkg/nvme-driver/server/src/main.cc
- pkg/l4virtio/include/virtio_block.h
```

**关键接口**:
```cpp
// VirtIO块设备操作
- 设备初始化: l4virtio_set_status()
- 队列配置: l4virtio_config_queue()
- 注册数据空间: l4virtio_register_ds()
- 通知处理: l4virtio_device_notification_irq()
```

### 2. 完善ext4文件系统服务器 (`pkg/filesystem/ext4/server/`)

**目的**: 提供完整的ext4文件系统操作接口

**需要完善ext4_server.cc**:
```c
// 当前stub实现需要替换为:
- mount(const char *path): 使用ext4_mount()挂载文件系统
- umount(const char *path): 使用ext4_umount()卸载
- open/read/write/close: 实现文件操作
- opendir/readdir: 实现目录操作
- stat/fstat: 实现文件状态查询
- mkdir/rmdir: 实现目录管理
- 创建/删除文件操作
```

**需要完善ext4_blockdev.cc**:
```c
// 连接VirtIO块设备
- 实现blockdev接口函数
- 连接到virtio-block-driver
- 提供底层读写接口
```

**lwext4 API使用**:
```c
// pkg/filesystem/ext4/contrib/include/ext4.h
ext4_mount()    // 挂载文件系统
ext4_umount()   // 卸载文件系统
ext4_open()     // 打开文件
ext4_read()     // 读取文件
ext4_write()    // 写入文件
// 完整API参考lwext4文档
```

### 3. 磁盘格式化工具 (`pkg/mkfs.ext4/`)

**目的**: 创建和格式化ext4文件系统

**需要实现**:
```bash
# pkg/mkfs.ext4/server/src/main.c
#include <ext4_mkfs.h>
#include <ext4_blockdev.h>

// 命令行工具:
mkfs.ext4 /dev/virtio-blk0  // 格式化块设备

// 支持选项:
- --block-size: 块大小 (1024/2048/4096)
- --label: 文件系统标签
- --force: 强制格式化
```

**构建集成**:
```makefile
# 可以在构建脚本中自动格式化
# build.sh: 添加格式化步骤
qemu-img create -f qcow2 build/virt_disk.img 100M
# 使用主机mkfs.ext4预格式化，或实现QEMU内格式化工具
```

### 4. 系统启动配置 (`conf/ext4-test.cfg`)

**目的**: 系统启动时自动挂载文件系统

**配置文件结构**:
```lua
-- conf/ext4-test.cfg
local L4 = package.loaded["L4"] or require("L4")
local l   = L4.default_loader

-- 1. 启动VirtIO块设备驱动
l:start({
  caps = {
    -- 注册virtio设备管理器
  },
}, "rom/virtio-block-driver")

-- 2. 自动格式化磁盘（首次运行）
-- l:start({
--   args = { "/dev/virtio-blk0", "--force", "--label=turingos" },
-- }, "rom/mkfs.ext4")

-- 3. 启动ext4文件系统服务器
l:start({
  caps = {
    -- 传递块设备capability
    blockdev = virtio_blk_cap,
    -- 文件系统服务capability
  },
}, "rom/ext4fs")

-- 4. 启动native_shell并挂载文件系统
l:start({
  caps = {
    -- 传递文件系统capability
    fs = ext4fs_cap,
    rtc = rtc_ch,
    sigma0 = L4.Env.sigma0,
  },
}, "rom/native_shell")
```

### 5. native_shell文件系统集成

**目的**: shell命令访问文件系统

**需要添加的命令**:
```c
// pkg/native_shell/server/src/commands.cc
void cmd_mount(int argc, char **argv) {
    // mount <device> <mountpoint>
}

void cmd_umount(int argc, char **argv) {
    // umount <mountpoint>
}

void cmd_ls(int argc, char **argv) {
    // 列出文件目录
}

void cmd_cat(int argc, char **argv) {
    // 显示文件内容
}

void cmd_mkdir(int argc, char **argv) {
    // 创建目录
}

void cmd_touch(int argc, char **argv) {
    // 创建文件
}

void cmd_rm(int argc, char **argv) {
    // 删除文件
}

void cmd_stat(int argc, char **argv) {
    // 文件状态信息
}
```

**文件解析器更新**:
```c
// pkg/native_shell/server/src/shell_exec.cc
// 更新File_resolver支持ext4文件系统路径解析
// 支持: ext4://path/to/file 或 /ext4/path/to/file
```

## 建议实现顺序

### 阶段1: 基础块设备支持 (优先级P0)
1. 创建`pkg/virtio-block-driver/`
2. 实现基本的VirtIO块设备驱动
3. 验证设备能被正确识别和初始化
4. 编写基础测试

### 阶段2: 文件系统服务 (优先级P0)
1. 完善`ext4_blockdev.cc`连接到块设备驱动
2. 实现基本的mount/umount功能
3. 实现简单的文件操作（open/read/write）
4. 测试文件系统能正常挂载

### 阶段3: 格式化工具 (优先级P1)
1. 创建`pkg/mkfs.ext4/`
2. 实现磁盘格式化工具
3. 集成到构建流程或运行时自动格式化
4. 测试创建的文件系统能正常挂载

### 阶段4: Shell集成 (优先级P1)
1. 为native_shell添加文件系统命令
2. 更新shell_exec支持ext4路径
3. 测试基本的文件管理功能

### 阶段5: 启动配置 (优先级P2)
1. 创建`conf/ext4-test.cfg`
2. 配置启动时自动挂载文件系统
3. 集成到主启动流程
4. 测试完整的启动流程

### 阶段6: 高级功能 (优先级P3)
1. 实现完整的文件系统API
2. 添加权限管理
3. 支持符号链接、硬链接等
4. 文件系统性能优化

## 技术参考文档

### 现有实现参考
- **AHCI驱动**: `pkg/ahci-driver/server/src/main.cc`
- **NVME驱动**: `pkg/nvme-driver/server/src/main.cc`
- **块设备库**: `pkg/libblock-device/include/block_device_mgr.h`
- **VirtIO库**: `pkg/l4virtio/include/virtio_block.h`
- **ext4库**: `pkg/filesystem/ext4/contrib/include/ext4.h`

### API文档
- **lwext4 API**: `pkg/filesystem/ext4/contrib/include/ext4.h`
- **L4Re块设备**: `pkg/libblock-device/include/`
- **VirtIO规范**: QEMU文档 VirtIO Block Device

### 测试工具
- **L4Re测试框架**: `pkg/libblock-device/test/`
- **网络测试**: `tools/tcp_client.py` (参考结构)
- **现有文件工具**: `pkg/filesystem/`下其他文件系统

## 预期效果

完成实现后，系统将具备：
1. **100MB持久化存储**: 系统重启后数据不会丢失
2. **ext4文件系统**: 支持完整的文件系统功能
3. **Shell访问**: 用户可以通过shell管理文件
4. **程序访问**: 运行时程序可以读写文件
5. **自动挂载**: 系统启动时自动挂载文件系统

## 风险和注意事项

1. **VirtIO兼容性**: 确保QEMU VirtIO块设备实现与L4Re驱动兼容
2. **性能考虑**: 块设备IO性能可能影响整体系统性能
3. **错误处理**: 需要完善的错误处理和恢复机制
4. **内存管理**: 文件系统操作需要合理管理缓存和内存
5. **线程安全**: 多任务环境下需要保证文件系统操作的线程安全

## 总结

实现完整的ext4文件系统支持是一个多阶段工程，需要逐步完成块设备驱动、文件系统服务、格式化工具、启动配置和shell集成。采用分阶段实现可以降低复杂度，每个阶段都可以独立测试和验证。

建议从VirtIO块设备驱动开始，这是整个文件系统功能的基础。完成基础功能后，再逐步添加高级特性。