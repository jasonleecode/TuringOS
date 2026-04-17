# TuringOS Native Shell

## 概述

Native Shell 是 TuringOS 的**默认启动程序，也是唯一的交互入口**。系统上电后直接进入 shell，其他程序均从 shell 发起。

```
TuringOS Native Shell
Type 'help' for available commands.

turingos>
```

## 构建

```bash
# 构建 native shell（virt 平台）
make -C build/l4re_virt PKGS=native_shell

# 构建完整系统（含 shell 的引导镜像）
make -C build/l4re_virt
```

## 启动

```bash
# 默认启动（进入 native shell）
./run_qemu_virt.sh

# 带网络启动（用于 TCP server 等）
./run_qemu_virt.sh --net-tcp
```

退出 QEMU：`Ctrl-A X`

## 可用命令

### 通用

| 命令      | 说明                              |
|-----------|-----------------------------------|
| `help`    | 列出所有可用命令                  |
| `echo`    | 输出文本，`-n` 不换行             |
| `info`    | 打印 OS / 架构信息                |
| `uname`   | 打印内核和架构信息                |
| `clear`   | 清屏                              |
| `history` | 显示命令历史                      |
| `exit`    | 退出 shell                        |

### 文件系统

| 命令      | 说明                              |
|-----------|-----------------------------------|
| `pwd`     | 显示当前目录                      |
| `cd`      | 切换目录                          |
| `ls`      | 列出目录内容                      |
| `cat`     | 打印文件内容                      |
| `mkdir`   | 创建目录                          |
| `rm`      | 删除文件                          |

### 系统

| 命令               | 说明                                           |
|--------------------|------------------------------------------------|
| `env`              | 打印环境变量                                   |
| `date`             | 显示当前时间（需要 RTC）                       |
| `date -s "YYYY-MM-DD HH:MM:SS"` | 设置时间（需要 RTC capability） |

## 功能特性

- **readline 支持**：命令行编辑（方向键、退格）、Tab 补全命令名、历史记录
- **引号和转义**：支持单引号、双引号和反斜杠转义
- **RTC 集成**：`date` 命令通过 L4Re capability 访问 RTC 服务

## 从 Shell 启动程序

> 计划中。Shell 作为系统唯一入口，后续将支持通过 `exec` 或类似命令加载并启动其他已打包进 ROM 的 L4Re 程序。

## Boot 配置

Native shell 的启动项定义在 `l4mk/conf/modules.list`：

```
entry native-shell
roottask moe --init=rom/native_shell
module l4re
module native_shell
```

`default-entry native-shell` 保证系统默认加载此入口，无需手动指定。

如需带 RTC 启动：

```
entry native-shell-rtc
roottask moe rom/native-shell-rtc.cfg
module l4re
module ned
module native-shell-rtc.cfg
module virt-rtc.io
module io
module rtc
module native_shell
module libreadline.so
```
