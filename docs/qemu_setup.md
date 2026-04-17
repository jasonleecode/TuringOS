# QEMU 测试环境

## 快速开始

```bash
# 构建
make -C build/l4re_virt

# 运行（默认进入 native shell）
./run_qemu_virt.sh
```

退出 QEMU：`Ctrl-A X`

## 平台参数

| 参数     | 值                            |
|----------|-------------------------------|
| 模拟 CPU | Cortex-A15 (ARMv7-A)          |
| 内存     | 256 MB                        |
| UART     | PL011                         |
| 中断控制 | GICv2                         |
| 启动入口 | `native-shell`（默认）        |

## 启动选项

```bash
# 默认（native shell）
./run_qemu_virt.sh

# 启用网络，主机端口 5555 → 客户机 5000
./run_qemu_virt.sh --net-tcp

# 自定义主机端口
./run_qemu_virt.sh --net-tcp --host-port 8080
```

镜像选择规则：默认优先 `bootstrap_native-shell.elf`；`--net-tcp` 时优先 `bootstrap_tcp-server.elf`。

## 预期启动输出

```
L4 Bootstrapper
  RAM: 0000000040000000 - 000000004fffffff: 256.0 MiB
  Loading fiasco
  Loading sigma0
  Loading moe
  Starting kernel fiasco at ...

TuringOS Native Shell
Type 'help' for available commands.

turingos>
```

## 网络测试

```bash
# 终端 1：启动 QEMU
./run_qemu_virt.sh --net-tcp

# 终端 2：连接
python3 tools/tcp_client.py --port 5555
```

## 调试

### QEMU 调试日志

```bash
qemu-system-arm -M virt -cpu cortex-a15 -m 256M \
    -kernel build/l4re_virt/images/bootstrap_native-shell.elf \
    -nographic -serial mon:stdio \
    -d guest_errors,unimp,cpu_reset -D /tmp/qemu.log
```

### GDB 远程调试

```bash
# 终端 1：等待 GDB 连接
qemu-system-arm -M virt -cpu cortex-a15 -m 256M \
    -kernel build/l4re_virt/images/bootstrap_native-shell.elf \
    -nographic -serial mon:stdio -s -S

# 终端 2
arm-linux-gnueabihf-gdb build/kernel_virt/fiasco.debug
(gdb) target remote :1234
(gdb) continue
```

## 常见问题

**无输出或 CPU reset**：使用了错误平台的内核，重新构建 `make -C build/l4re_virt`。

**模块移动警告** `Moving up to N modules behind 41100000`：正常行为，bootstrap 自动调整模块位置。
