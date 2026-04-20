# BeagleBone Black (AM335x) 适配说明

## 已完成的工作

### 主仓库

| 文件 | 说明 |
|------|------|
| `conf/bbb-hw.io` | IO 服务硬件配置（UART0、GPIO1、I2C1、RTC） |
| `conf/bbb-native-shell.cfg` | ned 启动配置：io → rtc server → native_shell |

### 子模块（需在各子模块仓库中单独提交）

**kernel 子模块：**
- `kernel/src/templates/globalconfig.out.arm-omap3-am33xx`
  - Cortex-A8 内核模板，启用 `CONFIG_PF_OMAP=y`、`CONFIG_PF_OMAP3_AM33XX=y`
- `kernel/src/kern/arm/bsp/omap/` 下已有 OMAP3/AM33xx BSP，包含 UART、timer、PIC 支持

**l4mk 子模块：**
- `l4mk/conf/platforms/omap3_am33xx.conf`
  - RAM 基址 `0x80000000`（256 MB），UART0 `0x44e09000`，GPIO1 `0x4804c000`，I2C1 `0x4802a000`

**驱动包：**
- `pkg/i2c-driver/server/src/am335x.h` — AM335x I2C 控制器驱动（基于 TRM SPRUH73 Ch21）
- `pkg/spi-driver/server/src/am335x.h` — AM335x McSPI 控制器驱动（基于 TRM SPRUH73 Ch24）
- `pkg/rtc/server/src/am335x_rtc.cc` — AM335x RTC 驱动（基于 TRM SPRUH73 Ch20）
- `pkg/io/io/server/src/drivers/gpio/omap.cc` — OMAP3/AM33xx GPIO 驱动（寄存器布局兼容）
- `pkg/bootstrap/server/src/platform/omap.cc` — AM33xx 时钟初始化（CM_WKUP/CM_PER）

---

## 硬件地址参考（AM335x TRM SPRUH73）

| 外设 | 基址 | IRQ | 时钟寄存器 |
|------|------|-----|-----------|
| UART0 | 0x44E09000 | 72 | CM_WKUP + 0xB4 |
| GPIO1 | 0x4804C000 | 96 | CM_PER + 0xAC |
| I2C1 | 0x4802A000 | 71 | CM_PER + 0x48 |
| McSPI0 | 0x48030000 | 65 | CM_PER + 0x4C |
| RTC | 0x44E3E000 | 75 | CM_WKUP（已上电） |
| Timer1（1ms）| — | 67 | CM_PER + 0x80 |

---

## 构建方法

```bash
# 交叉编译器要求 arm-linux-gnueabihf-，GCC 11+
export CROSS_COMPILE=arm-linux-gnueabihf-

# 构建全部
./build.sh --board bbb all
```

构建产物：
- `build/kernel_arm/fiasco` — Fiasco.OC 微内核
- `build/l4re_arm/images/bootstrap_bbb.elf` — 可烧录的 ELF 镜像

---

## 待完成工作

### Pinmux 初始化（需真实硬件验证后再做）

AM335x 外设引脚复用需通过 Control Module（基址 `0x44E10000`）配置。
U-Boot 在移交控制权前通常已完成基本 pinmux，但若系统不经 U-Boot 冷启动，需在 bootstrap 里补充：

| 引脚功能 | 寄存器偏移 | 目标模式 |
|---------|-----------|---------|
| UART0_RXD | 0x970 | mode 0，输入，上拉 |
| UART0_TXD | 0x974 | mode 0，输出 |
| I2C1_SCL (SPI0_D1) | 0x958 | mode 2，输入，上拉 |
| I2C1_SDA (SPI0_CS0) | 0x95C | mode 2，输入，上拉 |

建议先上板用 U-Boot 启动验证基本串口，确认 pinmux 行为后再决定是否需要在 bootstrap 里添加。

### SPI 硬件配置

`bbb-hw.io` 和 `bbb-native-shell.cfg` 目前未覆盖 McSPI0（基址 `0x48030000`，IRQ 65）。
需要时按照 I2C 的方式添加设备声明和 vbus。

### 在真实 BBB 硬件上启动验证

所有地址和配置基于 TRM 推导，尚未经过真实硬件测试。
建议通过 JTAG 或 UART0 serial console 观察 bootstrap 输出，确认内核正常启动。
