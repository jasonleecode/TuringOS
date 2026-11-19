# 启动配置参考

TuringOS 使用 L4Re 的 `modules.list` 机制管理启动项。virt 的开发配置默认启动 `native-shell`。

## 文件位置

| 文件 | 作用 |
|------|------|
| `conf/modules.list` | 声明所有启动项和各项所需的模块 |
| `conf/*.cfg` | Ned/Lua 启动脚本，描述进程拓扑和 capability 分配 |
| `conf/boards/*.conf` | 板型、内核模板、L4Re 平台和 shell 入口 |
| `pkg/turingos-interfaces/include/` | 独立导出的用户态 IPC 协议头 |

## 当前启动项

| 入口 | 用途 |
|------|------|
| `native-shell` | QEMU virt 开发系统 |
| `bbb-native-shell` | AM335x GPIO、I²C 和 RTC |
| `imx6ul-native-shell` | i.MX6UL 无盘开发系统 |
| `rpi4-native-shell` | Raspberry Pi 4 串口系统 |
| `minimal` | 关闭可选产品包后的最小 Ned 系统 |
| `smp-spawn-bench-ci` | 双核调度回归测试 |

板型决定默认 shell 入口。产品配置与驱动配置保存在相同的板级 `.kconfig` 中。BBB 和 RPi4 的串口登录由 authd 验证。打包前将设备的凭据文件放入 `$BUILD_OUT/credentials/<板型>/board-shadow`（默认输出根为 `build`）。每行格式为 `user:salt_hex:hash_hex`，hash 为 SHA-256(salt 字节 + 密码字节)。构建会将哈希文件嵌入 ROM，未配置时镜像中的账号列表为空。i.MX6UL 的无盘开发入口显式开启开发账号。设置 `ENTRY=lvgl-demo` 等值可选择其他入口。

## modules.list 语法

```
default-entry <名称>          # 默认构建的入口

entry <名称>
roottask moe --init=rom/<程序>   # 直接启动单个程序
module l4re
module <程序>
```

或使用 Ned 脚本启动多进程：

```
entry <名称>
roottask moe rom/<脚本>.cfg
module l4re
module ned
module <脚本>.cfg
module <程序1>
module <程序2>
```

### 模块选项

```
module[uncompress] vmlinuz          # 加载前解压
module[fname=virt.dtb] dtb/x.dtb   # 重命名
module[arch=arm64] binary           # 仅特定架构
```

## 添加新程序的流程

1. 在 `pkg/` 中声明包及其 `Control` 依赖。跨服务的协议头由 `turingos-interfaces` 导出。
2. 在 `conf/modules.list` 的入口内添加 `module <二进制名>`，并将提供该二进制的包名加入该入口的 `# build-packages`。L4Re 自动补齐传递依赖。
3. 如需在开机时启动服务，在对应 `.cfg` 中分配 capability 并启动。
4. 使用 `ENTRY=<入口> ./build.sh --board <板型> all` 构建。SMP 冒烟脚本默认使用临时空目录构建专用入口；已有镜像可用 `--image <路径>` 显式测试。

## Ned 启动脚本示例

多驱动场景（`conf/my-system.cfg`）：

```lua
local L4 = require("L4");

local svc_ch = L4.default_loader:new_channel();

L4.default_loader:start({
  caps = { svc = svc_ch:svr(), icu = L4.Env.icu, io = L4.Env.io },
  log  = { "DRV", "green" }
}, "rom/my_driver");

L4.default_loader:start({
  caps = { svc = svc_ch },
  log  = { "APP", "cyan" }
}, "rom/my_app");
```
