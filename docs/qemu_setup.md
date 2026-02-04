# QEMU 测试环境配置

## 概述

TuringOS 现在支持在 QEMU ARM Virt 平台上测试，这是用于开发和调试的推荐方式。

## 快速开始

```bash
# 1. 构建 virt 平台
./build.sh --board virt all

# 2. 运行 QEMU
./run_qemu_virt.sh
```

## 平台对比

### BeagleBone Black (BBB)
- **硬件**: AM335x SoC, Cortex-A8
- **RAM 基地址**: 0x81000000
- **UART**: AM335x 特定 UART
- **中断控制器**: AM335x INTC
- **QEMU 支持**: 无 (需要真实硬件或更复杂的模拟)

### QEMU Virt
- **模拟 CPU**: Cortex-A15 (ARMv7-A)
- **RAM 基地址**: 0x40000000
- **UART**: PL011
- **中断控制器**: GICv2
- **QEMU 支持**: 完整支持

## 构建配置

### Virt 平台构建
```bash
# 内核模板: arm-virt-pl1
# L4Re 配置: arm-virt-v7a
./build.sh --board virt all
```

### BBB 平台构建
```bash
# 内核模板: arm-omap3-am33xx
# L4Re 配置: arm-omap3-am33xx
./build.sh --board bbb all
```

## QEMU 命令详解

```bash
qemu-system-arm \
    -M virt \                    # 使用 ARM virt 机器类型
    -cpu cortex-a15 \            # 模拟 Cortex-A15 处理器
    -m 256M \                    # 256MB 内存
    -kernel bootstrap-image-virt.elf \  # 引导镜像
    -nographic \                 # 无图形界面
    -serial mon:stdio            # 串口输出到终端
```

## 启动输出解析

成功启动时，你会看到：

```
L4 Bootstrapper
  RAM: 0000000040000000 - 000000004fffffff: 256.0 MiB
  Loading fiasco
  Loading sigma0
  Loading moe
  Starting kernel fiasco at 40001288

Welcome to the L4Re Microkernel!
L4Re Microkernel on arm-32

GICv2
ARM generic timer: freq=62500000

SIGMA0: Hello!
MOE: Hello world
MOE: Starting: rom/hello
Hello World!
```

## 常见问题

### QEMU 无输出或 CPU reset

**原因**: 使用了错误平台的内核 (如 BBB 内核)

**解决**:
```bash
./build.sh --board virt all
```

### 找不到 mkimage

**影响**: 无法生成 uImage/ITB 格式 (不影响 ELF 格式)

**解决**:
```bash
# macOS
brew install u-boot-tools

# Ubuntu
sudo apt install u-boot-tools
```

### 模块移动警告

```
Moving up to 5 modules behind 41100000
```

**说明**: 这是正常行为，bootstrap 会自动调整模块位置以避免内存冲突。

**优化**: 在 `l4mk/conf/modules.list` 中设置 `modaddr 0x01100000`

## 调试技巧

### 启用 QEMU 调试日志

```bash
qemu-system-arm \
    -M virt -cpu cortex-a15 -m 256M \
    -kernel bootstrap-image-virt.elf \
    -nographic -serial mon:stdio \
    -d guest_errors,unimp,cpu_reset \
    -D /tmp/qemu_debug.log
```

### GDB 调试

```bash
# 终端 1: 启动 QEMU (等待 GDB 连接)
qemu-system-arm \
    -M virt -cpu cortex-a15 -m 256M \
    -kernel bootstrap-image-virt.elf \
    -nographic -serial mon:stdio \
    -s -S

# 终端 2: 启动 GDB
arm-linux-gnueabihf-gdb build/kernel_virt/fiasco.debug
(gdb) target remote :1234
(gdb) continue
```

## 退出 QEMU

- **方式 1**: 按 `Ctrl-A` 然后按 `X`
- **方式 2**: 在 QEMU monitor 中输入 `quit`
- **方式 3**: 从另一个终端 `killall qemu-system-arm`

## 下一步

- 尝试运行更复杂的应用
- 测试 L4Re 的 IPC 机制
- 运行 L4Linux 虚拟机
- 开发自定义 L4Re 应用

## 文件位置

- **构建产物**: `build/artifacts/`
- **内核**: `fiasco-virt` (527 KB)
- **引导镜像**: `bootstrap-image-virt.elf` (1.0 MB)
- **运行脚本**: `run_qemu_virt.sh`
