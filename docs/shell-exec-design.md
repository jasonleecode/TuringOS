# shell-exec 设计文档

## 1. 背景与定位

### 1.1 当前系统状态

TuringOS 的 native_shell 提供了一个功能丰富的用户态 shell，但原先只能执行内置命令（如 `ls`、`cat`、`date` 等），无法从 shell 中动态启动和运行其他 L4Re 任务。

**原有架构限制**：
```
用户在 native_shell 提示符中输入命令
  ↓
命令解析为内置函数调用
  ↓
直接执行对应的 C 函数
  ↓
输出结果到终端
```

这种实现的局限性：
- 无法运行 ROM 中的可执行程序（如 `hello`、`fb-test` 等）
- 缺乏进程管理能力（启动、等待、终止子任务）
- 无法传递参数或环境变量给新程序
- 与 L4Re 微内核的多任务能力脱节

**当前系统存储现状**：
- **ROM 文件系统**：L4Re Boot_fs，只读，编译时打包，存储核心系统程序
- **文件系统移植中**：正在为系统移植 ext4 文件系统，支持持久化存储和读写操作
- **未来存储规划**：程序文件可能存储在 ROM、ext4 文件系统或其他存储介质上

### 1.2 shell-exec 的定位

`shell-exec` 是为 native_shell 添加的程序加载与执行功能，使其能够：
- 从 ROM 文件系统加载并执行 ELF 可执行文件（已实现）
- 从 ext4 文件系统加载并执行 ELF 可执行文件（未来支持）
- 实现基本的任务生命周期管理（启动、等待、退出）
- 支持参数传递和简单的工作目录继承
- 为未来更完整的 POSIX 兼容层奠定基础

**存储策略与可扩展性设计**：
- **ROM 存储**：系统核心程序，启动时加载，不可修改
- **ext4 存储**：用户程序和数据，动态加载，可读写
- **统一抽象接口**：设计抽象的文件加载接口，屏蔽底层存储差异
- **自动路径解析**：根据路径前缀（`rom/`、`/` 等）自动选择存储后端

**实现后的架构**：
```
用户在 native_shell 提示符中输入 "run hello arg1 arg2"
  ↓
检测到 'run' 命令 → 调用 Simple_task_manager::spawn()
  ↓
路径解析：
  - "rom/hello" → 从 Boot_fs 加载
  - "/usr/bin/hello" → 从 ext4 加载（未来）
  ↓
使用 L4Re libloader 加载 ELF
  ↓
通过 Factory 创建新 Task/Thread/Region-map
  ↓
设置栈、参数、环境变量
  ↓
启动子任务，等待其完成
  ↓
返回退出码给 shell
```

---

## 2. L4Re 程序加载机制

### 2.1 L4Re 的任务创建流程

在 L4Re 中创建新任务涉及以下核心组件：

| 组件 | L4 协议 | 用途 |
|------|---------|------|
| `L4::Task` | 0x3e | 任务控制块，代表一个地址空间 |
| `L4::Thread` | 0x3c | 线程控制块，执行单元 |
| `L4Re::Rm` | 0x4005 | 区域映射器，管理虚拟内存布局 |
| `L4::Factory` | 0x4000 | 工厂接口，创建对象 |
| `L4::Ipc_gate` | 0x0 (Kobject) | IPC 门，用作 parent 接口接收子任务退出信号 |

**完整流程**：
1. **初始化**：通过 `L4Re::Env::env()->user_factory()` 创建子 Task、Thread、Rm；通过 `env()->factory()` 创建 IPC gate（parent 接口）
2. **加载 ELF**：使用 `l4re/libloader` 解析程序头，加载段到内存
3. **设置栈**：为子任务分配栈空间，推入参数和环境变量
4. **映射资源**：将初始 capability（log、mem_alloc、scheduler、factory、rm、parent）映射到子任务
5. **启动线程**：设置入口点和调度器参数，调用 `run_thread()`

### 2.2 L4Re ELF 加载器

L4Re 提供了 `l4/libloader/loader` 库，核心类包括：

- **`Ldr::Loader`**: 通用加载器基类，提供 `launch()` 接口
- **`Ldr::Elf_loader<App_model, Dbg>`**: ELF 格式专用加载器，解析 PT_LOAD 段
- **`Ldr::Base_app_model<Stack>`**: 应用模型基类，管理栈和程序信息
- **`Ldr::Remote_app_model<Base>`**: 远程应用模型，提供 `alloc_prog()`、`start_prog()` 等

**关键数据结构**：
```cpp
struct Prog_start_info {
  l4_addr_t utcbs_start;      // UTCB 区域起始地址
  l4_addr_t kip;              // KIP 地址
  l4_umword_t ldr_flags;
  l4_umword_t l4re_dbg;

  l4_fpage_t parent;          // parent capability（IPC gate）
  l4_fpage_t mem_alloc;       // 内存分配器
  l4_fpage_t scheduler;       // 调度器
  l4_fpage_t log;             // log 输出
  l4_fpage_t factory;         // 对象工厂
  l4_fpage_t rm;              // 区域映射器
};
```

### 2.3 libloader 模板约束（实现关键点）

`Remote_app_model<Base>` 通过 `this->method()` 调用 `Base` 中的方法，因此**所有被 `Remote_app_model` 调用的方法必须定义在 `Base` 类中**，不能只定义在派生类里。

这意味着不能直接写：
```cpp
// 错误：Base_app_model<Shell_stack> 没有 Dataspace 类型，
// Remote_app_model::prog_attach_stack(typename Base::Dataspace) 无法编译
class Simple_app_model : public Ldr::Remote_app_model<Ldr::Base_app_model<Shell_stack>>
```

必须引入中间层，这也是 ned 采用的模式：
```cpp
// ned 的模式：App_model 继承 Base_app_model 并定义 Dataspace，
// 再 typedef Remote_app_model<App_model> Rmt_app_model
struct Shell_model_base : public Ldr::Base_app_model<Shell_stack> {
  typedef L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap Dataspace;
  typedef L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap Const_dataspace;
  // ... 所有 Remote_app_model 通过 this-> 调用的方法和成员变量
};

typedef Ldr::Remote_app_model<Shell_model_base> Simple_app_model;
```

### 2.4 `l4re_aux` 初始化

`Shell_model_base::local_kip_ds()` 需要 `l4re_aux->kip_ds` 来获取 KIP dataspace cap。
`l4re_aux` 不由 uclibc 启动代码自动设置，每个需要使用它的程序必须自己解析 AUX vector，
与 ned 的做法相同：

```cpp
// main.cc
l4re_aux_t const *l4re_aux = nullptr;  // 全局定义，shell_exec.cc 用 extern 引用

int main(int argc, char const* const* argv)
{
    // AUX vector 跟在 envp 之后（标准 L4Re 启动栈布局）
    auto auxp = &argv[argc] + 1;
    while (*auxp) ++auxp;   // 跳过 envp 各项
    ++auxp;                  // 跳过 envp 的 NULL 结束符
    auto *sentinel = reinterpret_cast<char const*>(0xf0);  // L4_AUX_TYPE_KINFO
    while (*auxp) {
        if (*auxp == sentinel)
            l4re_aux = reinterpret_cast<l4re_aux_t const*>(auxp[1]);
        auxp += 2;
    }
    // ...
}
```

### 2.5 现有实现参考

#### 2.5.1 MOE 的加载器

位置：`l4re/moe/server/src/loader.cc`

**特点**：
- 作为 root task 运行，可以直接访问 sigma0
- 通过 `Boot_fs` 从 ROM 读取文件
- 实现了完整的 App_model，支持虚拟内存映射
- 通过 Lua 配置启动多个子任务

#### 2.5.2 ned 的应用模型

位置：`l4re/ned/server/src/app_model.h`

**结构**：
```cpp
struct App_model : public Ldr::Base_app_model<Stack> {
  typedef L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap Const_dataspace;
  typedef L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap Dataspace;
  // ...
};
typedef Ldr::Remote_app_model<App_model> Rmt_app_model;
```

shell-exec 的 `Shell_model_base` / `Simple_app_model` 直接参照此模式，但去掉了 ned 的 App_task、Foreign_server 等服务端基础设施，改用简单的 `l4_ipc_receive()` 等待退出信号。

---

## 3. 实现方案

### 3.1 总体设计

在 native_shell 中实现一个简化的程序加载器，核心思路：

1. **不重复造轮子**：复用 `l4re/libloader` 库
2. **最小化依赖**：避免引入 ned 的服务端基础设施，直接使用原生 L4 API
3. **渐进式实现**：先支持基本功能，再逐步完善

**架构分层**：
```
┌─────────────────────────────────────┐
│  native_shell 主循环 (main.cc)       │
│  - 解析 AUX vector，初始化 l4re_aux  │
│  - 查询 "programs" namespace        │
└─────────────────┬───────────────────┘
                  │ 简单命令解析
    ┌─────────────▼─────────────┐
    │  内置命令 (commands.cc)    │
    │  - help, ls, cat, date... │
    │  - run → spawn + wait     │
    └─────────────┬─────────────┘
                  │ 新增 'run' 命令
    ┌─────────────▼─────────────┐
    │  shell_exec (新增模块)     │
    │  - File_resolver           │
    │  - Shell_model_base        │
    │  - Simple_app_model        │
    │  - Simple_task_manager     │
    └─────────────┬─────────────┘
                  │ 调用 L4Re libloader
    ┌─────────────▼───────────────────────────┐
    │  L4Re libloader + L4::Kernel API        │
    │  - Ldr::Elf_loader<Simple_app_model>    │
    │  - L4::Factory, Task, Thread, Ipc_gate  │
    │  - L4Re::Rm, Mem_alloc, Env_ns          │
    └─────────────────────────────────────────┘
```

### 3.2 核心组件

#### 3.2.1 文件存储抽象层

```cpp
enum class File_backend { ROM, EXT4, MEMORY };

class File_descriptor {
public:
  virtual ~File_descriptor() = default;
  virtual L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap load_as_dataspace() = 0;
  virtual size_t  size()   const = 0;
  virtual bool    exists() const = 0;
  virtual File_backend backend() const = 0;
  const char* path() const;
  const char* name() const;
};

class Rom_file : public File_descriptor {
  // 通过 Env_ns 查询 rom/<name>，持有 Ref_cap<Dataspace>
};

// 未来：
class Ext4_file : public File_descriptor { ... };

class File_resolver {
public:
  static void set_programs_library(L4::Cap<L4Re::Namespace> ns);
  static File_descriptor* resolve(const char* path);
  static bool file_exists(const char* path);
  // 三参数版本，避免与 POSIX basename(char*) 符号冲突
  static char* basename(const char* path, char* buf, size_t sz);
private:
  static L4::Cap<L4Re::Namespace> g_programs_ns;
  static L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap open_from_env(const char* name);
};
```

**路径解析策略**：
- 任何输入：先尝试 `Env_ns.query("rom/<name>")`，再尝试 `Env_ns.query("<name>")`，最后查 `g_programs_ns`
- 第五阶段升级：`/` 开头的绝对路径优先走 ext4，回退到 ROM

#### 3.2.2 Shell_model_base / Simple_app_model

```cpp
struct Shell_model_base : public Ldr::Base_app_model<Shell_stack>
{
  typedef L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap Dataspace;
  typedef L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap Const_dataspace;
  enum { Utcb_area_start = 0xb3000000UL };

  Shell_model_base();
  void set_args(int argc, const char* const* argv, const char* const* envp);
  void release_caps(l4_cap_idx_t *task, l4_cap_idx_t *thread,
                    l4_cap_idx_t *rm,   l4_cap_idx_t *gate);

  // --- libloader 接口 ---
  Const_dataspace open_file(const char* name);
  Dataspace alloc_ds(unsigned long size) const;
  Dataspace alloc_ds_aligned(unsigned long size, unsigned align) const;
  Dataspace alloc_app_stack();
  void init_prog();
  void prog_attach_ds(...);
  int  prog_reserve_area(...);
  l4_addr_t local_attach_ds(...) const;
  void      local_detach_ds(...) const;
  static void copy_ds(...);
  static void ds_map_info(Const_dataspace ds, l4_addr_t *start);
  static bool all_segs_cow() { return false; }
  static Const_dataspace reserved_area() { return {}; }
  static Dataspace     local_kip_ds();   // 使用 l4re_aux->kip_ds
  static L4::Cap<void> local_kip_cap();
  void get_task_caps(L4::Cap<L4::Factory>*, L4::Cap<L4::Task>*, L4::Cap<L4::Thread>*);
  l4_cap_idx_t push_initial_caps(l4_cap_idx_t start);
  void         map_initial_caps(L4::Cap<L4::Task>, l4_cap_idx_t);
  l4_msgtag_t  run_thread(L4::Cap<L4::Thread>, l4_sched_param_t const&);

private:
  L4Re::Util::Unique_del_cap<L4::Task>     _child_task_cap;
  L4Re::Util::Unique_del_cap<L4::Thread>   _child_thread_cap;
  L4Re::Util::Unique_del_cap<L4Re::Rm>     _child_rm;
  L4Re::Util::Unique_del_cap<L4::Ipc_gate> _parent_gate;
  int _argc; const char* const* _argv; const char* const* _envp;
};

typedef Ldr::Remote_app_model<Shell_model_base> Simple_app_model;
```

**loader-side 内存分配陷阱**：

`alloc_ds()` / `alloc_app_stack()` 在 `init_prog()` 之前被调用，此时
`prog_info()->mem_alloc` 尚未设置（为空 fpage），必须使用**父侧**的 mem_alloc：

```cpp
// 正确：使用父进程自己的 mem_alloc
L4Re::chksys(L4Re::Env::env()->mem_alloc()->alloc(size, mem.get()), "alloc_ds");

// 错误：init_prog() 之前 prog_info()->mem_alloc.raw == 0，导致 "Void capability invoked"
L4::Cap<L4Re::Mem_alloc> ma(prog_info()->mem_alloc.raw & L4_FPAGE_ADDR_MASK);
```

`init_prog()` 负责将父侧的各种 capability fpage 写入 `prog_info()`，供子任务的 L4Re 运行时使用。

#### 3.2.3 Simple_task_manager

```cpp
class Simple_task_manager {
public:
  struct Task_handle {
    l4_cap_idx_t task_cap, thread_cap, rm_cap, parent_gate_cap;
    char name[64];
    bool running;
  };

  Task_handle* spawn(const char* program, char* const* argv, char* const* envp);
  int  wait(Task_handle* task);   // l4_ipc_receive() 在 parent_gate 上阻塞
  void kill(Task_handle* task);   // unmap + L4_FP_DELETE_OBJ

private:
  static constexpr int MAX_TASKS = 16;
  Task_handle _tasks[MAX_TASKS];
  int _task_count;
};
```

**退出信号机制**：子任务调用 `_exit(code)` 时，L4Re 运行时向 `env()->parent()` 发送
`signal(0, code)` IPC，即向 `_parent_gate` 发消息，message word[0]=sig=0，word[1]=exit_code。
父进程用 `l4_ipc_receive()` 接收并提取退出码。

#### 3.2.4 `run` 命令

```cpp
void cmd_run(int argc, char **argv) {
  if (argc < 2) { printf("Usage: run <program> [args...]\n"); return; }

  static Simple_task_manager task_manager;
  const char *program = argv[1];
  char *const *exec_argv = &argv[1];  // argv[0] = 程序名
  char *const *exec_envp = nullptr;   // 暂不继承环境变量（第三阶段实现）

  auto *task = task_manager.spawn(program, exec_argv, exec_envp);
  if (!task) { printf("run: failed to start %s\n", program); return; }

  // 当前：前台运行，同步等待退出
  // 第二阶段：支持 & 后台运行
  int exit_code = task_manager.wait(task);
  printf("run: %s exited with code %d\n", program, exit_code);
}
```

---

## 4. 阶段规划

### 第一阶段：最小可用产品（MVP）— ✅ 已完成

**目标**：实现最基础的程序加载功能

**功能列表**：
- [x] 从 ROM 加载并执行单一 ELF 程序
- [x] 抽象文件查找接口（ROM 后端，预留 ext4）
- [x] 传递基本参数（argv[0] = 程序名）
- [x] 子任务输出到同一 log capability
- [x] 同步 `wait()` 等待退出信号
- [x] `kill()` 强制终止子任务
- [x] 命令行接口：`run <program>`

**验收结果**：
```
turingos> run hello
[shell-exec] spawning: hello
[shell-exec] spawned: hello (task=4382720)
Hello World!
Hello World!
...
```

**工作量**：实际约 3 天（含调试 libloader 模板约束和 l4re_aux 初始化问题）

---

### 第二阶段：任务管理

**目标**：添加任务生命周期管理

**功能列表**：
- [ ] 后台任务支持（`run <prog> &` 语法）
- [ ] `jobs` 命令列出运行中的任务
- [ ] `wait <task-id>` 等待指定任务完成
- [ ] `kill <task-id>` 终止运行中的任务
- [ ] 任务退出码捕获和报告

**验收标准**：
```bash
native_shell> run hello &
[1] hello started
native_shell> jobs
[1]  running  hello
native_shell> wait 1
[1]  exited 0
```

**工作量估计**：2 天

---

### 第三阶段：参数和环境

**目标**：完善程序执行环境

**功能列表**：
- [ ] 支持命令行参数传递（argv 骨架已有，需验证子任务侧正确接收）
- [ ] 环境变量设置和继承（`exec_envp = environ`）
- [ ] 工作目录传递（虽然 L4Re 无完整 VFS）
- [ ] 重定向支持（`>`, `2>`, `<` 基础版）

**验收标准**：
```bash
native_shell> run myprog arg1 arg2  # 传递参数
native_shell> export DEBUG=1 && run myprog  # 继承环境
```

**工作量估计**：3 天

---

### 第四阶段：完善与优化

**目标**：提升用户体验和稳定性

**功能列表**：
- [ ] 错误处理和友好的错误消息
- [ ] 自动的 capability 清理（当前 `Unique_del_cap` RAII 已覆盖失败路径）
- [ ] 资源限制（堆大小、栈大小等）
- [ ] 性能优化（如 mmap 缓存）
- [ ] 文档和示例程序

**工作量估计**：2-3 天

---

### 第五阶段：ext4 文件系统集成（待 ext4 文件系统完成后）

**目标**：完成与 ext4 文件系统的深度集成，支持从持久化存储加载程序

**前置条件**：
- [ ] ext4 文件系统移植完成
- [ ] ext4 文件系统 IPC 接口已实现
- [ ] 基本的文件读写功能已验证

**功能列表**：
- [ ] 实现 `Ext4_file` 文件描述符类
- [ ] 支持 ext4 文件到 dataspace 的转换
- [ ] 实现路径解析策略（绝对路径优先 ext4）
- [ ] 支持 ext4 文件的执行权限检查
- [ ] 缓存机制（减少重复加载同一文件）
- [ ] 错误恢复（ext4 挂载失败时回退到 ROM）

**路径解析升级**：
```cpp
File_descriptor *File_resolver::resolve(const char *path) {
  if (strncmp(path, "rom/", 4) == 0) {
    return new Rom_file(path);
  } else if (path[0] == '/') {
    // 绝对路径：优先尝试 ext4
    auto *ext4_file = try_load_ext4(path);
    if (ext4_file && ext4_file->exists())
      return ext4_file;
    // ext4 失败，回退到 ROM（兼容性）
    return new Rom_file(path);
  } else {
    // 相对路径：当前目录（ext4）→ 系统路径 → ROM
    return search_path(path);
  }
}
```

**验收标准**：
```bash
native_shell> run /mnt/ext4/usr/bin/myapp
Task started: myapp (backend=ext4)
native_shell> run hello   # 仍从 ROM 加载
Task started: hello (backend=rom)
```

**工作量估计**：3-4 天

---

## 5. 技术细节

### 5.1 内存管理

**栈分配策略**：
- 固定栈大小：`Remote_stack<>` 默认值（通常 64KB）
- 栈地址：通过父侧 `env()->mem_alloc()` 分配，局部 attach 后写入 argv/envp，
  再通过子 Rm attach 到子地址空间固定位置
- 注意：loader-side 的 `alloc_ds()` / `alloc_app_stack()` 必须用父侧 mem_alloc，
  见 §3.2.2

**内存布局示例**：
```
0xc0000000 ┌─────────────────┐
            │   栈区域        │  64KB
            │   (向下增长)    │
            ├─────────────────┤
            │   程序段        │
            │   .text/.rodata │
            │   .data/.bss    │
            ├─────────────────┤
            │   进程堆        │  动态分配
            └─────────────────┘
0x00000000
```

**UTCB 区域**：
- 固定映射到 `Utcb_area_start = 0xb3000000`
- 大小：`1 << L4_PAGESHIFT`（一页，足够单线程使用）

### 5.2 Capability 映射

**`init_prog()` 设置的子任务初始 capability**：

| 名称 | 来源 | 子任务中的用途 |
|------|------|---------------|
| `log` | `env()->log()` | 标准输出 |
| `mem_alloc` | `env()->mem_alloc()` | 堆分配 |
| `scheduler` | `env()->scheduler()` | 线程调度 |
| `factory` | `env()->factory()` | 创建内核对象 |
| `rm` | `_child_rm`（新建） | 子任务自己的地址空间管理 |
| `parent` | `_parent_gate`（新建 IPC gate） | 退出信号通道 |

`Remote_app_model::start_prog()` 负责通过 `_ntask->map()` 将这些 fpage 映射到子任务的固定 cap 槽位（`Remote_app_std_caps` 中定义的槽号）。

**注意**：parent capability 类型为 `L4::Ipc_gate`（协议 0，`L4_PROTO_KOBJECT`），
不能用 `L4::Cap<void>` 创建，否则 `factory()->create<void>()` 报 "incomplete type" 编译错误。

### 5.3 线程与调度

**线程启动参数**（由 `Remote_app_model::start_prog()` 使用 `Remote_app_std_prios::Default_thread_prio = 2`）：

```cpp
l4_msgtag_t run_thread(L4::Cap<L4::Thread> thread,
                        l4_sched_param_t const &sp)
{
  // 从 prog_info()->scheduler fpage 提取 scheduler cap
  L4::Cap<L4::Scheduler> s(prog_info()->scheduler.raw & L4_FPAGE_ADDR_MASK);
  return s->run_thread(thread, sp);
}
```

**线程退出信号**：
- 子任务 `_exit(code)` → L4Re libc 调用 `env()->parent()->signal(0, code)`
- 父进程 `l4_ipc_receive(parent_gate_cap, l4_utcb(), L4_IPC_NEVER)` 阻塞等待
- 返回 tag 中：`mr[0] = sig = 0`，`mr[1] = exit_code`

### 5.4 ELF 加载细节

**支持的 ELF 类型**：
- 可执行文件（ET_EXEC）
- 位置无关可执行文件（ET_DYN，需动态链接器 `rom/l4re`）
- 共享库预留（暂不实现 `dlopen`）

**加载步骤**（由 `Ldr::Elf_loader::launch()` 完成）：
1. 调用 `open_file(name)` 获取 dataspace
2. 局部 attach 读取 ELF 头，验证 magic 和架构
3. 调用 `alloc_prog()` 创建子 Task/Thread
4. 调用 `prog_reserve_utcb_area()` 在子地址空间保留 UTCB 区域
5. 对每个 PT_LOAD 段：`alloc_ds()` 分配 + `copy_ds()` 复制 + `prog_attach_ds()` 映射
6. 调用 `alloc_app_stack()` 分配栈并局部映射以写入 argv/envp
7. 调用 `init_prog()` 设置 prog_info fpage，推入 argv/envp 字符串
8. 调用 `add_env()` 构造子任务的 `L4Re::Env` 结构并推到栈上
9. 调用 `start_prog()` 映射所有 cap，绑定 UTCB，启动线程

**启动栈布局（自底向上，Remote_stack 从高地址向低地址增长）**：
```
high addr  ┌─────────────────────────┐
            │  argv 字符串区域        │
            │  envp 字符串区域        │
            ├─────────────────────────┤
            │  L4Re::Env 结构体       │
            ├─────────────────────────┤
            │  initial caps 数组      │
            ├─────────────────────────┤
            │  l4re_aux 辅助信息      │
            ├─────────────────────────┤
            │  argc                   │
            │  argv 指针数组          │
            │  envp 指针数组          │
low addr   └─────────────────────────┘  ← sp
```

---

## 6. 接口与用法

### 6.1 命令行接口

**当前已实现**：
```bash
turingos> run hello          # 从 ROM 加载并等待退出
turingos> run rom/hello      # 等效（显式 rom/ 前缀）
```

**第二阶段后**：
```bash
turingos> run hello &        # 后台运行
[1] hello started
turingos> jobs
[1]  running  hello
turingos> wait 1
[1]  exited 0
turingos> kill 2
[2]  killed
```

### 6.2 程序接口（C++ API）

```cpp
#include "shell_exec.h"

Simple_task_manager mgr;
char *argv[] = { (char*)"hello", nullptr };
auto *task = mgr.spawn("hello", argv, nullptr);

if (!task) {
    fprintf(stderr, "Failed to spawn\n");
    return -1;
}

int exit_code = mgr.wait(task);
printf("Exit code: %d\n", exit_code);
```

---

## 7. 风险与挑战

### 7.1 技术风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| Capability 泄漏 | 资源耗尽 | `Unique_del_cap` RAII，失败路径已覆盖 |
| 内存映射冲突 | 段错误 | 使用子 Rm 的 `Search_addr` flag 自动分配 |
| loader-side 用错 mem_alloc | "Void capability invoked" | `alloc_ds()` 固定用 `env()->mem_alloc()` |
| `l4re_aux` 未初始化 | 链接错误或空指针 | main() 解析 AUX vector，同 ned |
| ELF 加载失败 | 无法启动程序 | `L4::Runtime_error` 异常捕获，输出详细错误 |
| 子任务不退出 | `wait()` 永久阻塞 | 第二阶段添加 `kill` 命令和超时机制 |

### 7.2 依赖关系

**必需依赖**：
- `l4re/libloader/lib`：ELF 加载器（需在 Control 和 Makefile 的 REQUIRES_LIBS 中声明）
- `l4re/include/l4/...`：L4 系统调用接口
- `l4re/include/l4re/...`：L4Re 运行时接口
- `<l4/sys/ipc_gate>`：`L4::Ipc_gate` 类型（parent gate 必须用此类型）

**可选依赖**：
- `cxx/ref_ptr`：智能指针（防止内存泄漏，当前用 `Unique_del_cap` 代替）

### 7.3 调试策略

**常用调试技巧**：
1. 启用 L4 调试输出：`l4re_dbg = L4.Dbg.Info`
2. 使用 `l4_debugger_add_image_info()` 注册符号信息
3. 监控 capability 使用：`l4util_show_caps()`
4. 检查内存映射：`task->map()` 返回值

**常见问题排查**：
```
问题：Void capability invoked (alloc stack / alloc_ds)
解决：alloc_ds/alloc_app_stack 中改用 env()->mem_alloc()，不要用 prog_info()->mem_alloc

问题：链接错误 undefined reference to 'l4re_aux'
解决：在 main.cc 定义全局 l4re_aux_t const *l4re_aux = nullptr 并解析 AUX vector

问题：编译错误 no type named 'Dataspace' in Base_app_model
解决：引入 Shell_model_base 中间层（见 §2.3）

问题：任务启动后立即 crash
解决：检查 ELF 入口点、栈对齐、capability 是否正确映射

问题：无法从 ROM 加载文件
解决：确认文件在 modules.list 中；open_file() 默认加 "rom/" 前缀查询 Env_ns

问题：子任务输出不显示
解决：确认 log capability 已通过 prog_info()->log fpage 映射到子任务
```

---

## 8. 未来扩展

### 8.1 POSIX 兼容性

一旦基础程序加载器工作正常，可以逐步添加 POSIX 语义：

| POSIX 概念 | L4Re 语义 |
|------------|-----------|
| `fork()` | 克隆 Task/Thread/Region-map |
| `execve()` | 加载新 ELF 入口点 |
| `waitpid()` | Parent IPC gate 等待（已实现基础版） |
| `pipe()` | 创建一对 IPC gate 连接 |
| `dup2()` | capability 重定向 |

### 8.2 高级功能

- **进程组**：使用 `L4::Cpu_mask` 管理 CPU 亲和性
- **信号机制**：扩展 Parent IPC 支持任意信号
- **命名空间**：为每个任务创建独立的命名空间
- **容器化**：隔离文件系统视图、网络等

### 8.3 多文件系统支持

| 文件系统 | 用途 | 优先级 |
|----------|------|--------|
| **ROM (Boot_fs)** | 系统核心程序，只读 | ✅ 已支持 |
| **ext4** | 用户数据和程序，可读写 | 高（进行中） |
| **FAT32** | 交换存储，Windows 兼容 | 中 |
| **tmpfs** | 临时文件，内存文件系统 | 中 |
| **NFS** | 网络文件系统，分布式存储 | 低 |

---

## 9. 构建配置

**`pkg/native_shell/server/src/Makefile`**：
```makefile
TARGET     = native_shell
SRC_CC     = main.cc commands.cc cmd_net.cc \
             devices.cc dev_temp.cc dev_rtc.cc dev_radio.cc \
             shell_exec.cc
REQUIRES_LIBS = readline rtc rtc_libc_be ds18b20 tef6686hn \
                lwip libc_be_socket_lwip libio-vbus libstdc++ l4util \
                libsigma0 devfs libloader
```

**`pkg/native_shell/Control`**：
```
requires: readline stdlibs compiler-rt rtc_libc_be rtc ds18b20 tef6686hn \
          lwip libc_be_socket_lwip libsigma0 devfs libloader
```

**`l4mk/conf/modules.list`**（native-shell entry）：
```
entry native-shell
roottask moe rom/native-shell.cfg
module l4re
module ned
module native-shell.cfg
module virt-rtc.io
module io
module rtc
module native_shell
module libreadline.so
module hello          ← 用于测试 run 命令
```

---

## 10. 总结

shell-exec 的核心价值在于：
1. 提升 TuringOS 的可用性（不再需要重启来测试不同程序）
2. 为开发和测试提供便利（快速迭代、A/B 测试）
3. 为更复杂的进程管理和 POSIX 兼容奠定基础
4. **支持多文件系统后端**，为 ext4 文件系统集成预留接口

### 设计原则

- **渐进式实现**：从 ROM 开始，逐步支持 ext4 和其他文件系统
- **抽象优先**：统一的文件接口，屏蔽底层存储差异
- **向后兼容**：保留 ROM 文件系统作为基础和回退选项
- **可扩展性**：预留接口，方便集成新的文件系统后端

### 当前状态与下一步

- **第一阶段完成**：`run hello` 在 QEMU virt 上验证通过
- **下一步**：第二阶段任务管理（`run &` / `jobs` / `kill`）
- **待解锁**：ext4 文件系统完成后实现 `Ext4_file`，无需修改任何上层代码
