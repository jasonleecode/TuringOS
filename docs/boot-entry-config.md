# 启动配置参考

TuringOS 使用 L4Re 的 `modules.list` 机制管理启动项。**默认启动项为 `native-shell`**，系统上电后直接进入交互式 shell。

## 文件位置

| 文件 | 作用 |
|------|------|
| `l4mk/conf/modules.list` | 声明所有启动项和各项所需的模块 |
| `conf/*.cfg` | Ned/Lua 启动脚本，描述进程拓扑和 capability 分配 |
| `l4mk/conf/Makeconf.boot` | 模块搜索路径和 QEMU 参数 |

## 当前启动项

| Entry | 说明 | 默认 |
|-------|------|------|
| `native-shell` | 直接启动交互式 shell | ✓ |
| `native-shell-rtc` | Shell + RTC 服务 + Ned 脚本 | |
| `tcp-server` | TCP echo server + 网络 | |

`default-entry native-shell` 写在 `l4mk/conf/modules.list` 中，构建时自动选择此入口。

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

1. 在 `pkg/` 下编写代码，按 L4Re 包结构组织
2. 构建：`make -C build/l4re_virt PKGS=<包名>`
3. 在 `l4mk/conf/modules.list` 的 `native-shell` 入口中追加 `module <程序名>`
4. 若需独立启动项，额外新增一个 `entry`

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
