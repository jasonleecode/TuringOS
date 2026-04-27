# ext4 文件系统实现计划

## 实现状态总览

| 阶段 | 内容 | 状态 |
|------|------|------|
| Phase 1 | VirtIO 块设备驱动 (`pkg/virtio-block-driver/`) | ✅ 完成 |
| Phase 2 | ext4fs 服务器 — 挂载 + I/O 自测 | ✅ 完成 |
| Phase 3b | L4Re Namespace 服务器 — POSIX 只读访问 | ✅ 完成 |
| Phase 4 | 写支持 — native_shell 可创建/修改 ext4 文件 | 🔲 待实现 |

---

## Phase 1 & 2：块设备驱动 + ext4 挂载（已完成）

### 架构

```
QEMU virtio-blk (bus=virtio-mmio-bus.1, 0xa000200)
    └─ io 服务 (virt-blk.io)
        └─ virtio-block-driver (pkg/virtio-block-driver/)
            └─ ext4fs 服务器 (pkg/filesystem/ext4/server/)
                ├─ 首次启动自动 mkfs.ext4
                ├─ ext4_mount("/")
                └─ 自测：写 /hello.txt，读回验证
```

### 关键文件

| 文件 | 说明 |
|------|------|
| `pkg/virtio-block-driver/server/src/main.cc` | VirtIO MMIO 枚举、libblock-device 集成 |
| `pkg/virtio-block-driver/server/src/virtio_device.cc` | `Block_dev`：init/reset/IRQ；关键修复：reset() 不写 QEMU MMIO |
| `pkg/filesystem/ext4/server/src/virtio_blockdev.cc` | lwext4 `ext4_blockdev` 接口，封装 L4virtio Client |
| `pkg/filesystem/ext4/server/src/main.cc` | 挂载、自测、Phase 3b 服务器循环 |
| `conf/virt-blk.cfg` | Ned 配置：io → blk-driver → ext4fs |
| `conf/virt-blk.io` | io 设备描述：SLOT0=net (0xa000000)，SLOT1=blk (0xa000200) |

---

## Phase 3b：L4Re Namespace 服务器（已完成）

### 原理

L4Re 的 libc VFS 是进程内机制。当 native_shell 调用 `fopen("/ext4/hello.txt")` 时：

```
fopen("/ext4/hello.txt")
  → Env_dir::get_entry("ext4")       # 从 env initial caps 查找 "ext4" cap
  → cap_to_vfs_object(ext4_cap)      # 识别为 L4RE_PROTO_NAMESPACE
  → File_factory<Namespace> → Ns_dir # 包装为 VFS 目录
  → Ns_dir::get_entry("hello.txt")
  → ext4fs::op_query("hello.txt")    # IPC 调用 ext4fs
    → ext4_fopen / ext4_fread        # 读取 lwext4
    → mem_alloc()->alloc(size, ds)   # 分配 Dataspace
    → 填充内容，返回只读 DS cap
  → File_factory<Dataspace> → Ro_file # 包装为只读内存映射文件
  → fread() 直接读映射内存
```

### 新增文件

| 文件 | 说明 |
|------|------|
| `pkg/filesystem/ext4/server/src/ext4_ns.h` | `Ext4_namespace` 类声明（实现 `L4Re::Namespace`） |
| `pkg/filesystem/ext4/server/src/ext4_ns.cc` | `op_query`：打开文件 → 分配 DS → 填充 → 返回只读 cap |
| `conf/virt-blk-shell.cfg` | 组合 Ned 配置：io + blk-driver + ext4fs(svr) + native_shell(ext4) |

### 局限性

- **只读**：`op_register_obj` 返回 `-EPERM`，native_shell 无法写入 ext4
- **全文件加载**：每次 `op_query` 将整个文件读入 Dataspace（不适合大文件）
- **无目录列举**：`ls /ext4/` 不工作（未实现 `getdents`）
- **无子目录**：`/ext4/dir/file` 的子命名空间未实现
- **cap 泄漏**：每次 `op_query` 会 `release()` 一个 Dataspace cap slot（受访问文件数限制）

---

## Phase 4：写支持（待实现）

### 目标

native_shell 能通过标准 POSIX 调用在 ext4 上创建和修改文件：

```
turingos> echo "test" > /ext4/new.txt   # 写入新文件
turingos> cat /ext4/new.txt             # 读回验证
test
```

### 方案

L4Re 的 `File_factory<Dataspace>` 创建的是只读的 `Ro_file`。要支持写，需要在 ext4fs 侧实现一个**可写文件协议**。

#### 方案 A：可写 Dataspace（简单，适合小文件）

1. `op_query` 返回**可写** Dataspace（`L4_CAP_FPAGE_RW`）
2. 客户端写入 Dataspace 后调用 ext4fs 的 `commit(handle)` 接口
3. ext4fs 将 Dataspace 内容写回 lwext4

问题：需要自定义 `commit` IPC，客户端需要显式调用，不能透明地通过 POSIX write 触发。

#### 方案 B：自定义 L4Re File 协议（完整 POSIX，复杂）

实现一个自定义 IPC 协议（类似 `L4Re::Vfs::File`），包含：
- `op_open(path, flags)` → 返回 file handle
- `op_read(handle, offset, size)` → 数据 DS
- `op_write(handle, offset, data_ds, size)` → 写入并刷盘
- `op_close(handle)`

客户端注册对应的 `File_factory`，使 `fopen/fread/fwrite` 透明地走 IPC。

#### 方案 C：共享内存写缓冲（推荐，平衡复杂度）

1. `op_open(path, flags)` → 返回 `{read_ds, write_ds, handle}`
   - `read_ds`：初始文件内容（只读）
   - `write_ds`：预分配写缓冲（可写）
2. 客户端 `fwrite` → 写入 `write_ds`
3. 客户端 `fclose` 触发 `op_commit(handle, size)` → ext4fs 将 `write_ds` 写入磁盘
4. 通过 `File_factory` 注册使整个流程对 native_shell 透明

### 实现步骤

1. **定义 IPC 接口** (`pkg/filesystem/ext4/include/ext4_file_iface.h`)
   - `L4_RPC` 定义：`open`, `read`, `write`, `commit`, `close`

2. **实现服务端** (`ext4_file_server.cc`)
   - 管理 open file 表（handle → ext4_file + write_ds）
   - `op_write` 或 `op_commit` 时调用 `ext4_fwrite`

3. **实现客户端库** (`pkg/filesystem/ext4/client/`)
   - `File_factory` 注册自定义协议
   - `Be_file` 子类封装 IPC 调用

4. **native_shell 链接客户端库**
   - `Makefile` 添加 `REQUIRES_LIBS += ext4_client`
   - 启动时 `vfs_ops->register_file_factory(...)` 注册工厂

---

## 构建与运行

### 构建

```bash
# 构建所有 virt 平台镜像（含 virt-blk-shell）
./build.sh --board virt bootstrap

# 或只构建 virt-blk-shell 入口
ENTRY=virt-blk-shell ./build.sh --board virt bootstrap

# 仅重建 ext4fs 二进制
make -C build/l4re_virt/ext-pkg/$(pwd)/pkg/filesystem/ext4/server/src \
     O=build/l4re_virt L4DIR=$(pwd)/l4mk

# 重新生成 ELF 镜像
export MODULE_SEARCH_PATH="build/kernel_virt:build/l4re_virt/bin/arm_armv7a/l4f:build/l4re_virt/lib/arm_armv7a/std/l4f:conf"
make -C build/l4re_virt E=virt-blk-shell elfimage
```

### 运行

```bash
# ext4 文件系统 + native_shell（推荐）
./run_qemu_virt.sh --cfg virt-blk-shell --disk 100M --no-net

# 仅块设备驱动测试
./run_qemu_virt.sh --cfg virt-blk --disk 100M --no-net

# native_shell 内操作 ext4 文件（Phase 3b 只读）
turingos> cat /ext4/hello.txt
Hello from TuringOS ext4fs!
```

> **注意**：`virt-blk.io` 将块设备固定在 `bus=virtio-mmio-bus.1`（MMIO 0xa000200，IRQ 49）。
> `run_qemu_virt.sh --disk` 自动使用正确的 bus 参数，手动运行 QEMU 时需指定
> `-device virtio-blk-device,drive=vdisk,bus=virtio-mmio-bus.1`。

---

## 已知问题与修复记录

| 问题 | 根因 | 修复 |
|------|------|------|
| `process_request` 永久挂起 | `Block_dev::reset()` 向 QEMU MMIO 写 status=0，清除 QueuePFN | `virtio_device.cc`: reset() 不写 MMIO |
| ext4fs 收不到 IRQ | `op_device_notification_irq` 发送 `L4_CAP_FPAGE_RO` | 改为 `L4_CAP_FPAGE_RW` |
| ELF 镜像缺少 ext4fs | `modules.list` virt-blk 条目漏掉 `module ext4fs` | 已补充 |
| QEMU DeviceID=0 | 未指定 `bus=virtio-mmio-bus.1`，块设备落在错误 MMIO 槽 | run 脚本始终指定 bus |
