# TuringOS

基于 [Fiasco](https://l4re.org/) 微内核构建的操作系统。Fiasco 是一个 L4 家族的微内核，TuringOS 在其之上通过 L4Re 运行时环境提供驱动和应用程序支持。

<img width="731" height="837" alt="1770173627060" src="https://github.com/user-attachments/assets/edbfe07f-9838-4da2-aceb-39474c17de3c" />


## 目录结构

```
turingos/
├── kernel/          # Fiasco 微内核源码 (Git 子模块)
├── l4re/            # L4Re 运行时环境 (Git 子模块)
├── l4mk/            # L4Re 构建系统入口 (启动项 conf/modules.list 在此)
├── pkg/             # 驱动、服务与应用（扁平结构，约 50+ 个包）
│   ├── native_shell/      #   交互式 Shell
│   ├── spawn/             #   spawnd 进程加载服务
│   ├── filesystem/ext4/   #   ext4 文件系统服务
│   ├── virtio-*/          #   VirtIO 块 / 网络 / 串口驱动
│   ├── fb-drv/ lvgl/      #   显示子系统
│   ├── benchmark/         #   cyclictest / ipcbench / smp-spawn-bench
│   └── ...                #   传感器、CAN、WebAssembly(wamr) 等
├── conf/            # 启动配置 (*.cfg)
├── docs/            # 设计文档与 todo
├── tools/           # 测试与 CI 脚本 (ci_smp_smoke.sh 等)
├── build.sh         # 构建脚本 (--board rpi4|bbb|virt|imx6ul)
├── run_qemu*.sh     # QEMU 启动脚本 (virt / rpi4 / imx6ul)
├── Kconfig          # 驱动配置定义
├── Makefile         # 顶层 Makefile (menuconfig / defconfig)
└── build/           # 构建产物输出目录
```

## 支持的目标板

| 目标板 | SoC / 架构 | `--board` 参数 | 备注 |
|--------|-----------|----------------|------|
| QEMU ARM Virt | Cortex-A15 (ARMv7) | `virt` | **主力开发 / 验证平台**，功能最完整（`run_qemu_virt.sh`） |
| Raspberry Pi 4B | ARM64 (AArch64) | `rpi4`（CLI 默认） | 支持 QEMU 模拟 |
| BeagleBone Black | AM335x / ARM | `bbb` | 仅支持真机部署 |
| i.MX6UL | Cortex-A7 (ARMv7) | `imx6ul` | QEMU `mcimx6ul-evk` |

## 环境依赖

- **交叉编译工具链**（上游要求 GCC 11+，本项目 virt/bbb/imx6ul 统一用 GCC 12）
  - RPi4 (ARM64): `aarch64-elf-gcc` (macOS: `brew install aarch64-elf-gcc`)
  - virt / BBB / imx6ul (ARMv7): `arm-linux-gnueabihf-gcc` (macOS: `brew tap messense/macos-cross-toolchains && brew install arm-unknown-linux-gnueabihf`)
- **GNU Make 4+** — macOS 自带 Make 3.81 过旧，需 `brew install make`（脚本会自动检测 `gmake`）
- **Perl** — 内核构建需要
- **flex / bison** — menuconfig 功能需要
- **QEMU**（可选）— `brew install qemu`，用于 virt / RPi4 模拟测试

可通过 `CROSS_COMPILE` 环境变量覆盖默认工具链前缀：

```bash
export CROSS_COMPILE=aarch64-none-elf-
```

## 快速开始

```bash
# 推荐：构建并在 QEMU 运行 virt 平台（功能最完整）
./build.sh --board virt all && ./run_qemu_virt.sh

# 完整构建 (CLI 默认 RPi4)
./build.sh

# 仅构建内核
./build.sh kernel

# 仅构建 L4Re 运行时
./build.sh l4re

# 仅生成引导镜像 (需先完成 kernel 和 l4re)
./build.sh bootstrap

# 交互式驱动配置
./build.sh menuconfig

# 清理所有构建产物
./build.sh clean
```

为 BeagleBone Black 构建时加 `--board bbb`：

```bash
./build.sh --board bbb           # BBB 全量构建
./build.sh --board bbb l4re      # 仅构建 BBB 的 L4Re
```

## QEMU 测试

**Virt 平台（推荐）** —— 功能最完整，含网络、ext4、显示、SMP：

```bash
./build.sh --board virt all          # 构建
./run_qemu_virt.sh                   # 启动 native-shell（默认，含网络）
./run_qemu_virt.sh --cfg cyclictest  # 启动指定配置
./run_qemu_virt.sh --help            # 查看全部选项（--gpu / --disk / --smp 等）
```

默认账号 `root` / `12345678`。退出按 `Ctrl-A X`。

**RPi4 平台**：

```bash
./run_qemu.sh            # 启动默认镜像
./run_qemu.sh <entry>    # 启动指定 entry
./run_qemu.sh --check    # 仅验证镜像结构，不启动 QEMU
./run_qemu.sh --timeout 5  # 设置超时（秒），0 为不限
```

> BBB (AM335x) 不支持 QEMU 模拟，使用 `--check` 可验证镜像结构。

## 真机部署（BeagleBone Black）

1. 构建 BBB 镜像：

   ```bash
   ./build.sh --board bbb
   ```

2. 将 ELF 转换为 U-Boot 可引导的 uImage：

   ```bash
   arm-linux-gnueabihf-objcopy -O binary build/bootstrap.elf bootstrap.bin
   mkimage -A arm -T kernel -C none -a 0x80000000 -e 0x80000000 -d bootstrap.bin uImage
   ```

3. 将 `uImage` 复制到 SD 卡 FAT 分区。

4. 在 U-Boot 控制台中执行：

   ```
   fatload mmc 0 0x80000000 uImage
   bootm 0x80000000
   ```

## 启动项配置

系统启动项通过 `conf/modules.list` 和 `conf/system.ned` 配置。详见 [docs/boot-entry-config.md](docs/boot-entry-config.md)。
