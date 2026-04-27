# ext4 文件系统实现计划

## 实现状态总览

| 阶段 | 内容 | 状态 |
|------|------|------|
| Phase 1 | VirtIO 块设备驱动 (`pkg/virtio-block-driver/`) | ✅ 完成 |
| Phase 2 | ext4fs 服务器 — 挂载 + I/O 自测 | ✅ 完成 |
| Phase 3b | L4Re Namespace 服务器 — POSIX 只读访问 | ✅ 完成 |
| Phase 4 | 写支持 — `echo > /ext4/file`、`cat` 读回 | ✅ 完成 |
| Phase 5 | 目录列举 — `ls /ext4` 及子目录递归 | ✅ 完成 |

---

## Phase 1 & 2：块设备驱动 + ext4 挂载（已完成）

### 架构

```
QEMU virtio-blk (bus=virtio-mmio-bus.1, 0xa000200)
    └─ io 服务 (virt-blk.io)
        └─ virtio-block-driver (pkg/virtio-block-driver/)
            └─ ext4fs 服务器 (pkg/filesystem/ext4/server/)
                ├─ 主机预格式化（run_qemu_virt.sh mkfs.ext4）
                ├─ ext4_mount("/")
                └─ 自测：写 /hello.txt，读回验证
```

### 关键文件

| 文件 | 说明 |
|------|------|
| `pkg/virtio-block-driver/server/src/main.cc` | VirtIO MMIO 枚举、libblock-device 集成 |
| `pkg/virtio-block-driver/server/src/virtio_device.cc` | `Block_dev`：init/reset/IRQ；关键修复：reset() 不写 QEMU MMIO |
| `pkg/filesystem/ext4/server/src/virtio_blockdev.cc` | lwext4 `ext4_blockdev` 接口，封装 L4virtio Client |
| `pkg/filesystem/ext4/server/src/main.cc` | 挂载、自测、服务器循环 |
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
    → Ext4_file_svr 分配共享 DS
    → 填充文件内容，返回 IPC gate cap
  → File_factory<Ext4_file_ops> → Ext4_file_vfs
  → fread/fwrite 直接操作映射内存
```

### 关键文件

| 文件 | 说明 |
|------|------|
| `pkg/filesystem/ext4/server/src/ext4_ns.h/.cc` | `Ext4_namespace`：op_query 分发文件/目录/dirinfo |
| `conf/virt-blk-shell.cfg` | 组合 Ned 配置：io + blk-driver + ext4fs(svr) + native_shell(ext4) |

---

## Phase 4：写支持（已完成）

### 目标

```
turingos> echo "test" > /ext4/new.txt
turingos> cat /ext4/new.txt
test
```

### 实现方案：共享 Dataspace + 延迟回写

1. `op_query(name)` → 创建 `Ext4_file_svr`，分配 R/W Dataspace，预填文件内容
2. 客户端 `Ext4_file_vfs` 映射 DS，`preadv`/`pwritev` 直接操作内存，`_dirty` 标记有改动
3. `fclose()` → `unlock_all_locks()` → `op_close(written)` → `ext4_fwrite` 写回磁盘
4. `O_TRUNC`（`echo >` 重定向）：`ftruncate(0)` → `_dirty=true, _written=0` → `op_close(0)` → 服务端 `ext4_fopen("wb")` 清空文件

### Shell 输出重定向

`parse_line` 把 `>` 当普通 token，在 `main.cc` 的命令分发循环里补充解析：
扫描 argv 找 `>` 和后续路径 → `open(O_WRONLY|O_CREAT|O_TRUNC)` → `dup2` 到 stdout → 执行命令 → 恢复 stdout。

### 关键文件

| 文件 | 说明 |
|------|------|
| `pkg/filesystem/ext4/include/ext4_file_proto.h` | `Ext4_file_ops` IPC 协议（0x5800）：`get_ds`、`close` |
| `pkg/filesystem/ext4/server/src/ext4_file_svr.h/.cc` | 服务端：DS 分配、预填、`op_close` 回写 |
| `pkg/filesystem/ext4/client/src/ext4_vfs.h/.cc` | 客户端：`Ext4_file_vfs`（Be_file_pos）+ `File_factory` 注册 |
| `pkg/native_shell/server/src/main.cc` | `>` 输出重定向逻辑 |
| `run_qemu_virt.sh` | 新建磁盘时自动 `mkfs.ext4 -b 4096 -O ^has_journal`（绕过 lwext4 mkfs bug） |

### 已知问题修复

| 问题 | 根因 | 修复 |
|------|------|------|
| `ext4_mkfs` 在全零磁盘上 segfault（PFA=0x13833003） | lwext4 mkfs 未初始化指针 | 主机用 `mkfs.ext4` 预格式化，服务端不再调用 mkfs |
| `Rpc_call::call` undefined reference | `L4_RPC_NF` 需要 `L4_RPC_DEF` 显式实例化 | 改用 `L4_INLINE_RPC_NF` |
| `off64_t` 未声明 | `_GNU_SOURCE` 在 `include lib.mk` 之前被 build system 剥除 | 将 `CPPFLAGS += -D_GNU_SOURCE` 移至 include 之后 |
| ext4fs 链接时 `__aeabi_unwind_cpp_pr0` 未定义 | ARM EABI 展开符号需要 libgcc | 添加 `libgcc libgcc_eh` 到 REQUIRES_LIBS |

---

## Phase 5：目录列举（已完成）

### 目标

```
turingos> ls /ext4
lost+found/
hello.txt
phase4.txt
turingos> ls /ext4/subdir
...
```

### 原理

`Ns_dir::getdents()` 通过查询特殊条目 `.dirinfo` 实现目录枚举：它调用 `_ns->query(".dirinfo", ds_cap)` 并期望收到一个 Dataspace，其内容为若干行 `<name-length>:<name>\n`。

```
ls /ext4
  → opendir("/ext4") → Ns_dir
  → readdir → Ns_dir::getdents
  → op_query(".dirinfo")
    → ext4_dir_open("/")
    → 枚举所有条目（跳过 "." ".."）
    → 构建 "<len>:<name>\n" 文本
    → 分配 DS，填充，返回 DS cap
  → 解析行，填充 dirent64 buf
  → 每个条目 stat() → op_query(name) → fstat
```

### 子目录支持

`op_query("subdir")` 在访问路径之前先用 `ext4_dir_open` 探测：
- 若是目录：注册一个新的 `Ext4_namespace(_registry, "/subdir")` 并返回其 cap（协议仍为 `L4Re::Namespace`，客户端自动包装为 `Ns_dir`，再次支持 `.dirinfo` 查询）
- 若是文件：走原来的 `Ext4_file_svr` 路径

`Ext4_namespace` 新增 `prefix` 构造参数（默认 `"/"`），子目录命名空间路径拼接为 `prefix + "/" + component`。

### 关键文件

| 文件 | 说明 |
|------|------|
| `pkg/filesystem/ext4/server/src/ext4_ns.h` | `Ext4_namespace` 新增 `_prefix` 成员和带 prefix 的构造函数 |
| `pkg/filesystem/ext4/server/src/ext4_ns.cc` | `make_dirinfo(dir_path)`：枚举目录并构建 dirinfo DS；`op_query` 三路分发：`.dirinfo` / 目录 / 文件 |

---

## 构建与运行

### 构建

```bash
# 重建 ext4fs + native_shell，打包镜像
make -C build/l4re_virt pkg/filesystem/ext4 pkg/native_shell
./build.sh --board virt bootstrap

# 仅重建 ext4fs
make -C build/l4re_virt pkg/filesystem/ext4
```

### 运行

```bash
# 首次运行：自动创建并 mkfs.ext4 格式化磁盘
./run_qemu_virt.sh --cfg virt-blk-shell --disk 100M --no-net

# 已有旧磁盘时强制重新格式化
rm build/virt_disk.img
./run_qemu_virt.sh --cfg virt-blk-shell --disk 100M --no-net
```

### 典型操作

```
turingos> echo "hello" > /ext4/hello.txt   # 写入
turingos> cat /ext4/hello.txt              # 读取
hello
turingos> ls /ext4                         # 目录列举
lost+found/
hello.txt
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
| ext4_mkfs segfault | lwext4 mkfs 在全零磁盘上空指针解引用（PFA=0x13833003） | 主机 mkfs.ext4 预格式化 |
| `ls /ext4` 返回空 | `Ns_dir::getdents` 需要 `.dirinfo` DS，服务端未实现 | Phase 5：`op_query(".dirinfo")` 枚举目录 |

---

## 后续方向

- **性能**：`op_query` 每次都分配 DS 并读取整个文件，大文件 / 高频访问时可考虑缓存或 mmap
- **写并发**：同一文件多次 `op_query` 会产生多个 `Ext4_file_svr`，末次 `op_close` 覆盖前面的写入
- **文件创建语义**：目前 `O_CREAT` 行为依赖 `ext4_fopen("wb")` 的副作用，未实现 `mkdir`
- **cap 生命周期**：`Ext4_file_svr` 和子 `Ext4_namespace` 被注册到 registry 后不会自动释放，长时间运行会耗尽 cap slot
