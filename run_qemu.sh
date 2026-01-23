#!/bin/bash
# 文件路径: run_qemu.sh

echo ">>> 正在 QEMU 中启动 Fiasco + L4Re 基础环境 ..."

# 使用 Mac 的 ARM 模拟器启动刚才生成的 bootstrap.elf
qemu-system-aarch64 \
    -M raspi4b \
    -m 2G \
    -nographic \
    -serial mon:stdio \
    -kernel build/bootstrap.elf

# 退出提示：按 Ctrl+A 然后按 X 退出