# spawn server 设计文档

## 1. 背景与动机

### 1.1 当前状态（Method C）

`shell-exec-design.md` 记录的第一阶段实现将 ELF 加载器直接嵌入 `native_shell`：

```
native_shell
  ├── shell_exec.cc  ← libloader 调用、Task/Thread/Rm 创建
  ├── commands.cc    ← run/jobs/kill 命令
  └── ...
```

这种方式能快速出结果，但有几个问题：
- **native_shell 越来越重**：ELF 加载、进程管理逻辑与 shell 本体耦合
- **无法服务其他客户端**：若将来有第二个 shell 或脚本引擎，加载能力无法复用
- **能力集中在一个进程**：native_shell 需要 factory、libloader 等高权限 cap，攻击面大
- **ext4 路径复杂**：加载 ELF 需要先从 ext4 读文件到 dataspace，在 native_shell 内做会混淆逻辑边界

### 1.2 目标（Method B）

将程序加载能力提取为独立的常驻服务 `spawnd`：

```
native_shell ──IPC──▶ spawnd ──创建──▶ 子任务
                        │
                        ├── factory cap  （向 moe 申请 Task/Thread/Rm）
                        ├── ext4 cap     （读 ELF 文件）
                        ├── log cap      （子进程日志）
                        └── syslogd cap  （集中日志）
```

native_shell 变成一个"薄客户端"：不需要 libloader，不需要 factory，只需要一个 `spawnd` cap。

---

## 2. 架构设计

### 2.1 组件关系

```
 ned (native-shell.cfg)
   ├── ext4fs ──────────────────────────────┐
   ├── syslogd                              │
   ├── spawnd  ←── factory, ext4, syslogd   │
   │     └── 子任务 A                        │
   │     └── 子任务 B                        │
   └── native_shell ←── spawnd cap          │
                         ext4 cap ──────────┘
```

### 2.2 启动顺序（native-shell.cfg）

```
1. fb-drv
2. io → rtc, virtio-block-driver
3. ext4fs
4. syslogd
5. spawnd        ← 新增，在 ext4fs 和 syslogd 之后
6. native_shell  ← 获得 spawnd cap
```

---

## 3. IPC 接口（`pkg/spawn/include/spawn_ipc.h`）

协议号 `0x5901`，三个方法：

```cpp
struct Spawn_svr : L4::Kobject_t<Spawn_svr, L4::Kobject, 0x5901>
{
    /* 启动程序。path 从 ext4 或 ROM 解析。
     * 成功返回 task handle（正整数），失败返回负错误码。
     * argv/envp 打包为 \0 分隔的字符串，以空字符串结尾。 */
    L4_INLINE_RPC(long, spawn,
        (L4::Ipc::Array<char const> path,
         L4::Ipc::Array<char const> args,
         l4_uint32_t                flags));

    /* 等待 handle 指定的子任务退出，返回退出码。
     * flags=0 阻塞；flags=1 非阻塞（WNOHANG）。 */
    L4_INLINE_RPC(long, wait,
        (l4_uint32_t handle,
         l4_uint32_t flags));

    /* 强制终止子任务，回收资源。 */
    L4_INLINE_RPC(long, kill,
        (l4_uint32_t handle));

    typedef L4::Typeid::Rpcs<spawn_t, wait_t, kill_t> Rpcs;
};
```

**flags 位定义（spawn）**：

| bit | 含义 |
|-----|------|
| 0   | `SPAWN_WAIT`   前台：spawn 完成后立即等待退出 |
| 1   | `SPAWN_BG`     后台：立即返回 handle，不等待 |
| 2   | `SPAWN_ROM`    强制从 ROM 加载（忽略 ext4）   |

**args 编码**：argv[0], argv[1], … 依次以 `\0` 分隔，末尾以额外 `\0` 结束。例如 `"hello\0arg1\0arg2\0\0"`。

---

## 4. spawnd 内部结构

### 4.1 整体代码结构

```
pkg/spawn/
  include/
    spawn_ipc.h          ← IPC 接口定义（客户端和服务端共用）
  server/
    src/
      main.cc            ← Registry_server 启动，注册 Spawnd 对象
      spawnd.cc          ← Spawnd::op_spawn / op_wait / op_kill
      spawnd.h
      app_model.cc       ← ELF 加载的 App_model（参照 ned）
      app_model.h
      task_table.cc      ← 子任务表，handle 分配与查找
      task_table.h
      Makefile
  Control
```

### 4.2 op_spawn 流程

```
op_spawn(path, args, flags)
  │
  ├─ 1. 解析 path：ext4 优先，回退 ROM
  │       ext4: ext4_fopen(path) → read → alloc dataspace → copy
  │       ROM:  Env_ns::query("rom/<name>") → dataspace
  │
  ├─ 2. 分配 handle = task_table.alloc()
  │
  ├─ 3. 构造 App_model（参照 ned 的 App_model，§5）
  │
  ├─ 4. Elf_loader<App_model>.launch(ds, argc, argv)
  │       → alloc Task/Thread/Rm（factory cap）
  │       → map PT_LOAD 段
  │       → alloc + push stack（argv/envp/env/aux）
  │       → map initial caps（log、syslogd、factory、rm、parent_gate）
  │       → run_thread()
  │
  ├─ 5. task_table.set_running(handle, task, thread, parent_gate)
  │
  └─ 6. if SPAWN_WAIT: 内部 wait(handle)，返回退出码
         else:          返回 handle
```

### 4.3 App_model

复用 `shell_exec.cc` 中已验证的 `Shell_model_base` / `Simple_app_model` 模式，搬入 `app_model.h`。主要差异：

| 项目 | native_shell 版 | spawnd 版 |
|------|-----------------|-----------|
| `open_file` | Env_ns 查 ROM | ext4 优先，回退 ROM |
| `init_prog` 中的 log cap | `env()->log()` | spawnd 自己的 log cap |
| `init_prog` 中的 syslogd cap | `env()->get_cap("syslogd")` | spawnd 的 syslogd cap |
| parent gate | 新建 IPC gate | 同，归 task_table 持有 |

### 4.4 子任务表（task_table）

```cpp
struct Child_task {
    l4_uint32_t                          handle;
    char                                 name[64];
    L4Re::Util::Unique_del_cap<L4::Task>     task;
    L4Re::Util::Unique_del_cap<L4::Thread>   thread;
    L4Re::Util::Unique_del_cap<L4Re::Rm>     rm;
    L4Re::Util::Unique_del_cap<L4::Ipc_gate> parent_gate;
    enum State { RUNNING, EXITED, KILLED } state;
    long exit_code;
};

class Task_table {
public:
    l4_uint32_t   alloc(const char *name);
    Child_task   *find(l4_uint32_t handle);
    void          set_running(l4_uint32_t handle, ...);
    void          set_exited(l4_uint32_t handle, long code);
    void          free(l4_uint32_t handle);
private:
    static constexpr int MAX = 32;
    Child_task _tasks[MAX];
    int        _count = 0;
};
```

### 4.5 op_wait 实现

```cpp
long op_wait(l4_uint32_t handle, l4_uint32_t flags)
{
    auto *t = task_table.find(handle);
    if (!t) return -L4_ENOENT;
    if (t->state == Child_task::EXITED) {
        long code = t->exit_code;
        task_table.free(handle);
        return code;
    }
    if (flags & WNOHANG) return -L4_EAGAIN;

    // 阻塞等待 parent_gate 上的退出信号
    l4_umword_t label;
    l4_msgtag_t tag = l4_ipc_receive(t->parent_gate.get().cap(),
                                     l4_utcb(), L4_IPC_NEVER);
    // mr[1] = exit_code（L4Re libc _exit 发送的信号）
    long code = l4_utcb_mr()->mr[1];
    task_table.free(handle);
    return code;
}
```

> **注**：当前 spawnd 是单线程服务器。多个并发 wait 请求会串行处理。
> 后续可用 `Br_manager_hooks` 加 IPC gate 将每个 wait 请求转为异步通知来改进。

---

## 5. native_shell 侧改造

### 5.1 shell_exec.cc 瘦身

移除 libloader 相关逻辑，替换为 spawnd IPC 调用：

```cpp
// 旧：inline ELF 加载（约 400 行）
// 新：
int Simple_task_manager::spawn(const char *path, char *const *argv, ...)
{
    // 打包 argv 为 \0 分隔字符串
    char args_buf[1024];
    pack_argv(args_buf, sizeof(args_buf), argv);

    long handle = g_spawnd->spawn(
        L4::Ipc::Array<char const>(strlen(path)+1, path),
        L4::Ipc::Array<char const>(args_len, args_buf),
        SPAWN_BG);

    return (handle > 0) ? (int)handle : -1;
}

int Simple_task_manager::wait(int handle)
{
    return (int)g_spawnd->wait((l4_uint32_t)handle, 0);
}
```

### 5.2 Makefile 简化

```makefile
# 移除 libloader；添加 spawn_client（只是 spawn_ipc.h，无需单独 lib）
REQUIRES_LIBS = readline rtc rtc_libc_be ... libklog
# libloader 不再需要
```

### 5.3 native-shell.cfg 新增 spawnd

```lua
-- 5. spawnd: ELF 加载服务
local spawnd_ch = nil
if ext4_ch then
  spawnd_ch = l:new_channel()
  l:start({
    caps = {
      svr     = spawnd_ch:svr(),
      ext4    = ext4_ch,
      syslogd = syslogd_ch,
    },
    log      = { "spawnd", "white" },
    l4re_dbg = L4.Dbg.Warn,
  }, "rom/spawnd")
end

-- native_shell 获得 spawnd cap
if spawnd_ch then shell_caps.spawnd = spawnd_ch end
```

---

## 6. ext4 文件加载

从 ext4 读 ELF 的路径（spawnd 侧）：

```cpp
L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap
App_model::open_from_ext4(const char *path)
{
    // 1. 通过 ext4 namespace cap 查找文件
    auto ext4_ns = L4Re::Env::env()->get_cap<L4Re::Namespace>("ext4");

    // 2. ext4fs 提供 open(path) → dataspace IPC
    //    使用 ext4_client 库的 Ext4_file_client::open()
    auto ds = alloc_ds(file_size);  // 先分配 dataspace
    ext4_read_file(ext4_ns, path, ds.get());  // 读入内容

    return ds;
}
```

> `ext4_client` 库已在 syslogd/native_shell 中使用。spawnd 的 Makefile 同样添加 `ext4_client` 依赖。

---

## 7. 实现计划

### 阶段 1：spawnd 骨架 + ROM 加载

**目标**：`run hello` 通过 spawnd 运行。

- [ ] `pkg/spawn/include/spawn_ipc.h`：定义 IPC 接口
- [ ] `pkg/spawn/server/src/main.cc`：Registry_server 框架
- [ ] `pkg/spawn/server/src/app_model.h/.cc`：搬移 Shell_model_base
- [ ] `pkg/spawn/server/src/task_table.h/.cc`：子任务表
- [ ] `pkg/spawn/server/src/spawnd.h/.cc`：op_spawn / op_wait / op_kill
- [ ] `pkg/spawn/server/src/Makefile` + `pkg/spawn/Control`
- [ ] `l4mk/pkg/spawn` 符号链接
- [ ] `l4mk/conf/modules.list` 加 `module spawnd`
- [ ] `conf/native-shell.cfg` 加 spawnd 启动 + cap 传递
- [ ] `pkg/native_shell` shell_exec.cc 改为 IPC 调用
- [ ] 验证：`run hello` 输出 "Hello World!"

### 阶段 2：wait / kill / jobs

- [ ] `wait <handle>` 命令
- [ ] `kill <handle>` 命令
- [ ] `jobs` 列出运行中任务（向 spawnd 查询状态）
- [ ] 后台运行：`run hello &`

### 阶段 3：ext4 加载

- [ ] spawnd 的 `open_from_ext4(path)` 实现
- [ ] 路径解析策略：`/ext4/...` → ext4，其他 → ROM
- [ ] 验证：将 hello 可执行文件写入 ext4 镜像，`run /ext4/hello`

### 阶段 4：子进程 I/O 与 cap 传递

- [ ] 将 native_shell 的 ext4 cap 转发给子进程（供子进程读写文件）
- [ ] syslogd cap 传递（子进程日志集中收集）
- [ ] stdin/stdout 重定向（基于 IPC gate pair）

---

## 8. 关键技术细节

### 8.1 spawnd 不能调用 `l4_ipc_receive` 等待子进程

spawnd 的主循环是 `Registry_server::loop()`，单线程处理 IPC。若在 `op_wait` 内
调用 `l4_ipc_receive(parent_gate, ...)` 阻塞，整个服务器都无法响应新请求。

解决方案（阶段 2）：
- `op_wait` 的阻塞版本在 native_shell 侧自己 `l4_ipc_receive`（传 parent_gate cap）
- 或：spawnd 为每个 wait 请求创建一个 helper 线程（复杂）
- 阶段 1 简化：只支持同步等待（spawn+wait 原子化），不支持分离的 wait 调用

### 8.2 IPC gate 所有权

`parent_gate` 由 spawnd 创建，子任务 exit 时向其发送退出信号。
spawnd 将 `parent_gate` 的客户端 cap 放入子任务的 caps 表（`env()->parent()`），
保留服务端在自己手里用于 `l4_ipc_receive`。

### 8.3 args 编码格式

```
spawn(path, args, flags)
  path: "/ext4/bin/myapp\0"          单个路径
  args: "myapp\0arg1\0arg2\0\0"      argv[0..n]，双 \0 结束
```

服务端解析：
```cpp
// 将 args buffer 拆成 char* 数组
static int unpack_args(const char *buf, size_t len,
                       char **argv, int max_argc)
{
    int argc = 0;
    const char *p = buf;
    const char *end = buf + len;
    while (p < end && *p && argc < max_argc) {
        argv[argc++] = const_cast<char*>(p);
        p += strlen(p) + 1;
    }
    argv[argc] = nullptr;
    return argc;
}
```

### 8.4 构建依赖

```
pkg/spawn/Control:
  provides: spawnd
  requires: stdlibs libstdc++ ext4_client libloader l4re-util

pkg/spawn/server/src/Makefile:
  TARGET        = spawnd
  SRC_CC        = main.cc spawnd.cc app_model.cc task_table.cc
  PRIVATE_INCDIR = $(SRC_DIR)/../../../spawn/include \
                   $(SRC_DIR)/../../../klog/include
  REQUIRES_LIBS = libstdc++ l4re-util l4util libloader ext4_client libklog
  LDFLAGS      += -u ext4_client_module_init
```

---

## 9. 与现有 shell-exec-design.md 的关系

| 维度 | Method C（当前，shell-exec-design.md）| Method B（本文，spawn server）|
|------|--------------------------------------|-------------------------------|
| ELF 加载位置 | native_shell 内部 | spawnd 独立进程 |
| native_shell 依赖 | libloader、factory cap | 只需 spawnd cap |
| 多客户端 | 不支持 | 任何进程都可调 spawnd |
| 权限隔离 | native_shell 权限高 | spawnd 最小权限原则 |
| 迁移路径 | shell_exec.cc 改为 IPC 调用 | 一次性切换 |

Method C 的 `App_model` 代码（已调试验证）**直接搬入 spawnd**，不丢弃。
