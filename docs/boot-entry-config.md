# TuringOS 启动项配置指南

## 概述

L4Re 的启动配置分三层：

| 层次 | 文件 | 作用 |
|------|------|------|
| 模块声明 | `conf/modules.list` | 声明一个启动项要打包哪些二进制 |
| 启动脚本 | `conf/*.ned` | Lua 脚本，定义进程拓扑、IPC 通道、硬件权限 |
| 构建路径 | `l4mk/conf/Makeconf.boot` | 告诉构建系统去哪里搜索内核和模块文件 |

## 第一层：modules.list

文件位置：`conf/modules.list`

每个 `entry` 定义一个可引导的系统镜像，包含内核和所有要加载的模块。

### 语法

```
entry <名称>
kernel <内核文件> [参数]
module sigma0
roottask moe rom/<启动脚本>
module <模块1>
module <模块2>
...
```

### 模块类型

| 类型 | 含义 |
|------|------|
| `kernel` | Fiasco 微内核 |
| `module sigma0` | 初始内存管理器（必需） |
| `roottask moe rom/<脚本>` | 根任务，Moe 启动后执行指定的 ned 脚本 |
| `module` / `bin` | 普通二进制模块（程序、驱动、库、配置文件） |
| `data` | 数据文件 |

### 模块选项（方括号语法）

```
module[uncompress] vmlinuz              # 加载前解压 gzip
module[fname=virt.dtb] dtb/xxx.dtb      # 重命名模块
module[arch=arm64] some_binary          # 仅特定架构包含
module[nostrip] debug_binary            # 不剥离符号
```

### 示例

最小启动项（仅内核 + ned 解释器）：

```
entry fiasco-base-test
kernel fiasco -serial_esc
module sigma0
roottask moe rom/ned
module l4re
module ned
```

完整启动项（带 IO 服务、驱动和应用）：

```
entry turingos-full
kernel fiasco -serial_esc
module sigma0
roottask moe rom/system.ned
module l4re
module ned
module io
module system.ned
module console_srv
module native_shell
```

### 架构过滤

可以限制某个 entry 只在特定架构下可用：

```
entry[arch=arm|arm64] my-arm-entry
kernel fiasco -serial_esc
...
```

## 第二层：Ned 启动脚本

文件位置：`conf/*.ned`（Lua 脚本）

Ned 是 L4Re 的 init 系统，用 Lua 描述进程启动顺序、IPC 连接和权限分配。

### 基本模式

```lua
local L4 = require("L4");

-- 1. 创建 IPC 通道
local channel = L4.default_loader:new_channel();

-- 2. 启动服务端进程（驱动）
L4.default_loader:start(
  {
    caps = {
      svc = channel:svr(),    -- 服务端权限
      icu = L4.Env.icu,       -- 中断控制器访问
      io  = L4.Env.io,        -- 硬件 IO 访问
    },
    log = { "SRV", "green" }
  },
  "rom/my_server"
);

-- 3. 启动客户端进程（应用）
L4.default_loader:start(
  {
    caps = {
      my_svc = channel,       -- 客户端权限
    },
    log = { "APP", "cyan" }
  },
  "rom/my_app"
);
```

### 关键 API

| API | 作用 |
|-----|------|
| `L4.default_loader:new_channel()` | 创建一条 IPC 通道 |
| `channel:svr()` | 获取通道的服务端权限 |
| `channel`（直接传递） | 客户端权限 |
| `L4.Env.icu` | 系统中断控制器 |
| `L4.Env.io` | 硬件 IO 资源 |
| `L4.default_loader:start(caps, binary)` | 启动一个进程 |

### 多驱动示例

```lua
local L4 = require("L4");

local console_ch = L4.default_loader:new_channel();
local gpio_ch    = L4.default_loader:new_channel();

-- 控制台驱动
L4.default_loader:start({
  caps = {
    svc = console_ch:svr(),
    icu = L4.Env.icu,
    io  = L4.Env.io,
  },
  log = { "UART", "green" }
}, "rom/console_srv");

-- GPIO 驱动
L4.default_loader:start({
  caps = {
    svc = gpio_ch:svr(),
    icu = L4.Env.icu,
    io  = L4.Env.io,
  },
  log = { "GPIO", "yellow" }
}, "rom/gpio_drv");

-- 应用程序，连接两个驱动
L4.default_loader:start({
  caps = {
    console = console_ch,
    gpio    = gpio_ch,
  },
  log = { "APP", "cyan" }
}, "rom/my_app");
```

## 第三层：Makeconf.boot

文件位置：`l4mk/conf/Makeconf.boot`

配置模块搜索路径和 QEMU 选项。

```makefile
# 内核和模块的搜索路径
MODULE_SEARCH_PATH += /path/to/kernel/build
MODULE_SEARCH_PATH += /path/to/conf

# QEMU 选项
QEMU_OPTIONS += -serial stdio
QEMU_OPTIONS += -nographic
```

构建系统会在 `MODULE_SEARCH_PATH` 中查找 `modules.list` 里声明的所有文件（fiasco、sigma0、l4re 等）。

## 构建和运行

### 生成引导镜像

```bash
# 在 L4Re 构建目录中，指定 entry 名称
cd l4re/build_arm64
CROSS_COMPILE=aarch64-elf- make E=fiasco-base-test elfimage

# BBB (ARM)
cd l4re/build_arm
CROSS_COMPILE=arm-none-eabi- make E=fiasco-base-test elfimage
```

### 通过 build.sh

```bash
# build.sh 中 build_bootstrap() 默认使用 E=fiasco-base-test
./build.sh bootstrap
./build.sh --board bbb bootstrap
```

### 查看可用启动项

`l4mk/conf/modules.list` 中的默认条目：

| Entry | 说明 |
|-------|------|
| `hello` | Hello World 示例 |
| `hello-cfg` | 带 Lua 配置的 Hello |
| `L4Linux-basic` | L4Linux (x86/amd64/arm/arm64) |
| `VM-basic` | 虚拟机运行 Linux (arm/arm64/amd64/riscv) |
| `VM-multi` | 多服务虚拟机 |
| `ipcbench` | IPC 性能基准测试 |

自定义条目在 `conf/modules.list` 中定义。

## 添加新程序/驱动的完整流程

1. **编写代码**，放在 `pkg/` 下，按 L4Re 包结构组织

2. **编译**，确保二进制出现在 `l4re/build_xxx/bin/` 下

3. **在 `conf/modules.list` 中注册**：
   ```
   entry my-system
   kernel fiasco -serial_esc
   module sigma0
   roottask moe rom/my-system.ned
   module l4re
   module ned
   module io
   module my-system.ned
   module my_driver
   module my_app
   ```

4. **编写 `conf/my-system.ned`**，定义进程拓扑和权限

5. **生成镜像**：
   ```bash
   cd l4re/build_arm && make E=my-system elfimage
   ```
