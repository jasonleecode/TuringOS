# TuringOS devfs 设计方案

> 目标：在 L4Re VFS 层实现 `/dev` 虚拟文件系统，让 native_shell 及所有用户态程序
> 可以通过标准 POSIX 接口（`open / read / write / ioctl`）访问系统设备。

---

## 1. 背景与目标

### 1.1 问题

当前硬件访问方式需要每个程序在启动时显式获取 L4Re capability（`vbus`、`rtc`、
`sigma0` 等），并调用驱动库的 C++ 接口。这意味着：

- 每个程序必须知道驱动的 capability 名字和 IPC 接口；
- 没有统一的设备发现机制；
- shell 无法用通用命令（`cat`、`echo`、`dd`）操作硬件；
- 外部程序（WAMR、vim 脚本等）无法使用标准 POSIX 设备接口。

### 1.2 目标

```
# 在 native_shell 中：
$ ls /dev/
  temp0   rtc0   radio0   net0   gpio0   tty0

$ cat /dev/temp0
24.35

$ echo "tune 96500" > /dev/radio0

$ dd if=/dev/gpio0 bs=1 count=1
```

所有程序通过 `open("/dev/xxx", ...)` 获得标准 fd，无需感知底层 L4Re 能力。

### 1.3 范围（第一阶段）

| 设备节点 | 对应硬件 | 操作 |
|---|---|---|
| `/dev/temp0` | DS18B20 温度传感器 | read：返回温度字符串 |
| `/dev/rtc0` | RTC 时钟 | read/write：获取/设置时间 |
| `/dev/radio0` | TEF6686HN 收音机 | read：状态；write：命令 |
| `/dev/tty0` | UART 串口 | read/write：透传 |
| `/dev/null` | 黑洞 | 标准语义 |
| `/dev/zero` | 零字节流 | 标准语义 |

网络设备（`/dev/net0`）、块设备（`/dev/mmcblk0`）留到第二阶段。

---

## 2. 现有架构分析

### 2.1 硬件访问分层

```
用户程序（native_shell / tcp_server / wamr）
    │ L4Re capability IPC
    ▼
服务层（io server / rtc server / i2c-driver server）
    │ 直接 MMIO
    ▼
硬件寄存器
```

驱动分两类：

- **库驱动**（ds18b20、tef6686hn）：编译进调用方，通过 vbus IPC 操作 GPIO/I2C。
- **服务驱动**（rtc、i2c-driver、io）：独立进程，暴露 IPC 接口，多个客户端共享。

### 2.2 L4Re VFS 层（实际 API）

L4Re 为用户态程序提供 POSIX 兼容层。其中 **`l4re_vfs`** 是文件系统抽象层，负责将
`open/read/write/close` 路由到对应的文件系统实现。

源码位置：`l4re/l4re_vfs/include/`

```
libc open("/dev/temp0")
    │
    ▼
l4re_vfs（全局 vfs_ops 单例）
    │ 查挂载树，"/dev" → Devfs_dir
    ▼
Devfs_dir::get_entry("temp0", ...)  → 返回 Temp_device_file
    │
    ▼
Temp_device_file::readv()
    │ 调用 ds18b20 库
    ▼
DS18B20 IPC → GPIO vbus → 硬件
```

**关键接口（已通过读源码确认）：**

```cpp
// 头文件：<l4/l4re_vfs/backend>
//         l4re/l4re_vfs/include/backend
//         l4re/l4re_vfs/include/vfs.h

namespace L4Re { namespace Vfs {

  // 全局单例，程序启动后由 libc 初始化
  extern Ops *vfs_ops;   // asm("l4re_env_posix_vfs_ops")

  // Ops::mount — 运行时动态挂载，无需静态注册
  // path: 挂载点路径，如 "/dev"
  // dir:  实现了 File 接口的目录对象
  class Ops {
    int mount(char const *path, cxx::Ref_ptr<File> const &dir) noexcept;
    // ...
  };

  // Be_file — 所有自定义文件/目录的基类（提供所有纯虚函数的默认实现）
  // new/delete 自动使用 vfs_ops->malloc/free
  class Be_file : public File {
    // 默认返回 -EINVAL 或 -ENOTDIR，子类只覆盖需要的方法
  };

  // Be_file_stream — 流式文件（无 seek），设备节点用这个
  // preadv/pwritev 自动转发给 readv/writev
  class Be_file_stream : public Be_file {
    virtual ssize_t readv(const struct iovec*, int)  noexcept override = 0;
    virtual ssize_t writev(const struct iovec*, int) noexcept override = 0;
  };

  // 目录路径解析的核心钩子（VFS 在 openat 中调用）
  // 子类覆盖此方法以实现目录查找
  class File {
    virtual int get_entry(const char *path, int flags, mode_t mode,
                          cxx::Ref_ptr<File> *f) noexcept;  // 默认返回 -ENOTDIR

    // ls 列目录时调用
    virtual ssize_t getdents(char *buf, size_t sizebytes) noexcept;
  };

} }
```

**挂载方式（动态，运行时调用即可）：**

```cpp
#include <l4/l4re_vfs/backend>

auto dir = cxx::make_ref_obj<Devfs_dir>();
L4Re::Vfs::vfs_ops->mount("/dev", dir);
// 之后任何 open("/dev/xxx") 都路由到 dir->get_entry("xxx", ...)
```

---

## 3. devfs 设计

### 3.1 包结构

新建 `pkg/devfs/`：

```
pkg/devfs/
├── Control                     # L4Re 构建系统包描述
├── include/
│   └── devfs/
│       ├── devfs.h             # 公开 API：init、register_device
│       └── device_file.h       # Device_file 基类
├── lib/
│   ├── build/
│   │   └── Makefile
│   └── src/
│       ├── devfs.cc            # Devfs_dir 实现 + init()
│       ├── device_registry.cc  # 设备注册表（名字 → Device_file）
│       └── dev_null.cc         # /dev/null、/dev/zero 内建实现
└── server/                     # 可选：独立 devd 进程（第二阶段）
```

**Control 文件：**

```
requires: l4re_vfs libc_be libstdc++
provides: devfs
```

### 3.2 设备注册接口

```cpp
// include/devfs/device_file.h
#pragma once
#include <l4/l4re_vfs/backend>   // Be_file_stream, Be_file

namespace Devfs {

  // 字符设备基类：继承 Be_file_stream（流语义，无 seek）
  // Be_file_stream 已提供所有纯虚函数的默认实现（返回 -EINVAL）
  // 子类只需覆盖需要的方法
  class Device_file : public L4Re::Vfs::Be_file_stream {
  public:
    // 子类覆盖：读设备（对应 POSIX read）
    ssize_t readv(const struct iovec *iov, int iovcnt) noexcept override
    { (void)iov; (void)iovcnt; return -ENOSYS; }

    // 子类覆盖：写设备（对应 POSIX write）
    ssize_t writev(const struct iovec *iov, int iovcnt) noexcept override
    { (void)iov; (void)iovcnt; return -ENOSYS; }

    // 子类覆盖：设备控制（对应 POSIX ioctl）
    int ioctl(unsigned long cmd, va_list args) noexcept override
    { (void)cmd; (void)args; return -ENOSYS; }

    // 默认：字符设备 stat（S_IFCHR, mode 0666）
    int fstat(struct stat64 *buf) const noexcept override;

    int get_status_flags() const noexcept override { return O_RDWR; }

    virtual ~Device_file() noexcept = default;
  };

} // namespace Devfs
```

```cpp
// include/devfs/devfs.h
#pragma once
#include "device_file.h"
#include <l4/cxx/ref_ptr>

namespace Devfs {

  // 初始化：将 devfs 挂载到 /dev（调用 vfs_ops->mount）
  // 必须在任何 open("/dev/...") 之前调用，通常在 main() 最开头
  int init();

  // 注册设备节点
  //   name:  节点名，如 "temp0"、"rtc0"（不含路径前缀）
  //   file:  Device_file 实例（引用计数，注册后由 devfs 持有）
  int register_device(const char *name, cxx::Ref_ptr<Device_file> file);

  // 注销设备节点（热拔插预留）
  int unregister_device(const char *name);

} // namespace Devfs
```

### 3.3 核心实现：Devfs_dir

`Devfs_dir` 是挂载到 `/dev` 的目录对象，继承 `Be_file`：

```cpp
// lib/src/devfs.cc（内部实现，不对外暴露）

#include <l4/l4re_vfs/backend>
#include <map>
#include <string>
#include <pthread.h>

class Devfs_dir : public L4Re::Vfs::Be_file {
public:
  // VFS 路径解析钩子：open("/dev/temp0") 最终调到这里，path = "temp0"
  int get_entry(const char *path, int /*flags*/, mode_t /*mode*/,
                cxx::Ref_ptr<L4Re::Vfs::File> *f) noexcept override
  {
    pthread_mutex_lock(&_lock);
    auto it = _devices.find(path);
    if (it == _devices.end()) {
      pthread_mutex_unlock(&_lock);
      return -ENOENT;
    }
    *f = it->second;
    pthread_mutex_unlock(&_lock);
    return 0;
  }

  // ls /dev/ 使用
  ssize_t getdents(char *buf, size_t sz) noexcept override;

  // stat("/dev") 使用
  int fstat(struct stat64 *buf) const noexcept override;

  int faccessat(const char *path, int /*mode*/, int /*flags*/) noexcept override
  {
    pthread_mutex_lock(&_lock);
    bool found = _devices.count(path) > 0;
    pthread_mutex_unlock(&_lock);
    return found ? 0 : -ENOENT;
  }

  // 注册/注销（供 Devfs::register_device 调用）
  int add(const char *name, cxx::Ref_ptr<Devfs::Device_file> f);
  int remove(const char *name);

private:
  std::map<std::string, cxx::Ref_ptr<Devfs::Device_file>> _devices;
  pthread_mutex_t _lock = PTHREAD_MUTEX_INITIALIZER;
  mutable off64_t _dents_pos = 0;   // getdents 游标
};

// 全局单例
static cxx::Ref_ptr<Devfs_dir> g_devfs_dir;
```

### 3.4 设备实现示例

#### /dev/temp0（DS18B20）

```cpp
class Temp_device_file : public Devfs::Device_file {
  Ds18b20 sensor_;
public:
  explicit Temp_device_file(L4vbus::Gpio_pin pin) : sensor_(pin) {}

  ssize_t readv(const struct iovec *iov, int iovcnt) noexcept override {
    int t = sensor_.read_temp_c100();
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%+d.%02d\n", t / 100, abs(t % 100));
    ssize_t copied = 0;
    for (int i = 0; i < iovcnt && copied < len; ++i) {
      size_t n = std::min((size_t)(len - copied), iov[i].iov_len);
      memcpy(iov[i].iov_base, buf + copied, n);
      copied += (ssize_t)n;
    }
    return copied;
  }
};
```

#### /dev/radio0（TEF6686HN）

```cpp
class Radio_device_file : public Devfs::Device_file {
  Tef6686hn radio_;
public:
  explicit Radio_device_file(L4::Cap<I2c_device_ops> i2c) : radio_(i2c) {}

  // read → 返回当前状态：{"band":"FM","freq":96500,"rssi":-45}\n
  ssize_t readv(const struct iovec *iov, int iovcnt) noexcept override;

  // write → 接受命令："tune FM 96500\n"、"seek up\n"、"init\n"
  ssize_t writev(const struct iovec *iov, int iovcnt) noexcept override;
};
```

#### /dev/rtc0

```cpp
class Rtc_device_file : public Devfs::Device_file {
  L4::Cap<L4rtc_iface> rtc_;
public:
  // readv  → "2026-04-17 14:30:00\n"
  // writev → 接受同格式字符串设置时间
  // ioctl  → RTC_RD_TIME / RTC_SET_TIME（struct rtc_time，兼容 Linux 号）
  ssize_t readv(const struct iovec*, int) noexcept override;
  ssize_t writev(const struct iovec*, int) noexcept override;
  int     ioctl(unsigned long, va_list)   noexcept override;
};
```

---

## 4. 启动流程变更

### 4.1 native_shell 初始化序列

```cpp
// pkg/native_shell/server/src/main.cc

#include <devfs/devfs.h>

int main() {
  // 1. 初始化 devfs（挂载 /dev）
  Devfs::init();

  // 2. 注册各设备（按能力可用性决定是否注册）
  setup_devices();   // 新增函数，见下

  // 3. 启动 readline shell（原有逻辑）
  run_shell();
}

static void setup_devices() {
  // /dev/null, /dev/zero（内建，无条件注册）
  Devfs::init_builtins();

  // /dev/temp0
  auto vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus");
  if (vbus.is_valid()) {
    L4vbus::Gpio_pin pin = find_gpio_pin(vbus, 4);
    Devfs::register_device("temp0",
      cxx::make_ref_obj<Temp_device_file>(pin), O_RDONLY);
  }

  // /dev/rtc0
  auto rtc = L4Re::Env::env()->get_cap<L4rtc_iface>("rtc");
  if (rtc.is_valid())
    Devfs::register_device("rtc0",
      cxx::make_ref_obj<Rtc_device_file>(rtc));

  // /dev/radio0
  auto i2c_factory = L4Re::Env::env()->get_cap<void>("i2c");
  if (i2c_factory.is_valid()) {
    auto i2c_dev = create_i2c_device(i2c_factory, TEF6686HN_I2C_ADDR);
    Devfs::register_device("radio0",
      cxx::make_ref_obj<Radio_device_file>(i2c_dev));
  }
}
```

### 4.2 ned Lua 配置

无需新增服务进程，只需确保现有能力正确传递给 native_shell：

```lua
-- conf/native-shell-full.cfg
local l = L4.default_loader

-- io server（提供 vbus：GPIO + I2C）
l:start({ caps = { ... } }, "rom/io rom/virt-full.io")

-- rtc server
l:start({ caps = { vbus = io_buses.vbus_rtc, rtc = rtc_ch:svr() } }, "rom/rtc")

-- native_shell（获取所有设备能力）
l:start({
  caps = {
    vbus  = io_buses.vbus_full,   -- GPIO + I2C
    rtc   = rtc_ch,
    i2c   = io_buses.i2c_factory,
    sigma0 = L4.Env.sigma0,       -- 为 net 命令保留
  }
}, "rom/native_shell")
```

---

## 5. 实现阶段规划

### 阶段一：基础框架（约 1 周）

- [ ] 新建 `pkg/devfs`，实现 `Devfs_mount`、`Device_registry`、`Devfs_dir`
- [ ] 实现 `/dev/null`、`/dev/zero` 内建节点
- [ ] 在 `l4re_vfs` 中注册挂载点
- [ ] 验证：`ls /dev/` 可列出目录，`cat /dev/null` 正常

### 阶段二：设备节点（约 1 周）

- [ ] `/dev/temp0`（DS18B20，只读）
- [ ] `/dev/rtc0`（RTC 读写 + `ioctl RTC_RD_TIME`）
- [ ] `/dev/radio0`（TEF6686HN 读写命令）
- [ ] 更新 native_shell `main.cc` 的设备注册代码
- [ ] 验证：`cat /dev/temp0` 输出温度

### 阶段三：shell 命令整合（约 3 天）

- [ ] `ls /dev/` 显示设备类型标记（`c` 字符设备）
- [ ] `cat /dev/xxx` 持续读取（Ctrl-C 退出）
- [ ] `echo "cmd" > /dev/radio0` 写入命令
- [ ] 移除 native_shell 中 `cmd_temp`、`cmd_radio` 对库的直接调用（改走 `/dev`）

### 阶段四：扩展（后续迭代）

- [ ] `/dev/tty0` UART 透传
- [ ] `/dev/gpio0`～`/dev/gpiоN` 单引脚控制
- [ ] `/dev/mmcblk0` 块设备（接 emmc-driver）
- [ ] `ioctl` 完整实现（兼容部分 Linux ioctl 号）
- [ ] 独立 `devd` 服务进程（支持多客户端共享设备）

---

## 6. 关键风险与注意事项

### 6.1 l4re_vfs 挂载 API（已确认）

通过读源码确认：

- `L4Re::Vfs::vfs_ops->mount(path, dir)` **完全支持运行时动态挂载**，在 `main()` 中
  直接调用即可，无需任何静态注册或 `__attribute__((constructor))`。
- `vfs_ops` 在 libc 的 `INIT_PRIO_VFS_INIT` 优先级构造函数中初始化，在 `main()` 到来
  前已就绪（`l4re/l4re_vfs/include/impl/default_ops_impl.h`）。
- 正确的基类是 `L4Re::Vfs::Be_file_stream`（流设备）和 `L4Re::Vfs::Be_file`（目录），
  而不是裸的 `L4Re::Vfs::File`（纯虚，需实现数十个方法）。
- 正确的头文件是 `<l4/l4re_vfs/backend>`。

### 6.2 线程安全

`device_registry_` 的读写需要加锁（native_shell 中 `net` 命令会启动后台线程）。
使用 `pthread_mutex_t` 或 L4Re 的 `l4util_atomic` 保护注册表。

### 6.3 读语义

温度传感器每次 `read()` 触发一次硬件采样（1-Wire 需 ~750ms）。
需要决定：
- **触发式**：每次 `read()` 采样（简单，但 `cat` 时阻塞 750ms）
- **缓存式**：后台定时采样，`read()` 返回缓存值（响应快，需后台线程）

第一阶段用触发式，后续改缓存式。

### 6.4 与现有 shell 命令的关系

`cmd_temp`、`cmd_radio` 命令保留（直接调库），`/dev/` 接口作为新增方式并行存在。
待 `/dev/` 稳定后再决定是否废弃旧命令。

---

## 7. 接口约定

### 设备节点读写格式

| 节点 | read 返回 | write 接受 |
|---|---|---|
| `/dev/temp0` | `+24.35\n` | 不支持 |
| `/dev/rtc0` | `2026-04-17 14:30:00\n` | `2026-04-17 14:30:00\n` |
| `/dev/radio0` | `{"band":"FM","freq":96500,"rssi":-45}\n` | `tune FM 96500\n` / `seek up\n` / `init\n` |
| `/dev/null` | 立即返回 0 | 吞掉数据 |
| `/dev/zero` | 返回 `\0` 字节流 | 吞掉数据 |

### ioctl 支持（第一阶段仅 rtc0）

```c
#include <linux/rtc.h>  // 兼容 Linux ioctl 号

ioctl(fd, RTC_RD_TIME, &rtc_time);   // 读取时间（struct rtc_time）
ioctl(fd, RTC_SET_TIME, &rtc_time);  // 设置时间
```

---

## 8. 文件变更一览

| 操作 | 路径 |
|---|---|
| 新建 | `pkg/devfs/` |
| 新建 | `pkg/devfs/include/devfs/devfs.h` |
| 新建 | `pkg/devfs/include/devfs/device_file.h` |
| 新建 | `pkg/devfs/lib/src/devfs.cc` |
| 新建 | `pkg/devfs/lib/src/device_registry.cc` |
| 新建 | `pkg/devfs/lib/src/dev_null.cc` |
| 新增 | `pkg/native_shell/server/src/devices.cc`（设备注册） |
| 修改 | `pkg/native_shell/server/src/main.cc`（调用 `Devfs::init()`） |
| 修改 | `pkg/native_shell/server/src/Makefile`（链接 devfs） |
| 修改 | `pkg/native_shell/Control`（requires devfs） |
| 新建 | `conf/native-shell-full.cfg` |
| 新建 | `conf/virt-full.io` |
