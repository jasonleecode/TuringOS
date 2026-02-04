# TuringOS 构建产物

构建时间: Wed Feb  4 10:24:47 CST 2026
目标板: BeagleBone Black (AM335x) (ARM)
交叉编译器: arm-linux-gnueabihf-gcc

## 文件列表

### 内核
- `fiasco-bbb` - Fiasco 微内核

### Bootstrap
- `bootstrap-bbb.elf` - Bootstrap 引导加载器 (ELF 格式)
- `bootstrap-bbb.raw` - Bootstrap 引导加载器 (RAW 二进制格式)
- `bootstrap-image-bbb.elf` - 包含所有模块的完整引导镜像
- `bootstrap-final-bbb.elf` - 最终引导镜像（包含内核和模块）

## 使用方法

### BeagleBone Black (AM335x)
1. 将 SD 卡格式化为 FAT32
2. 复制 `bootstrap-bbb.raw` 到 SD 卡
3. 通过 U-Boot 加载:
   ```
   fatload mmc 0 0x80000000 bootstrap-bbb.raw
   go 0x80000000
   ```

### Raspberry Pi 4
1. 准备 SD 卡并安装固件
2. 复制 `bootstrap-bbb.elf` 到 boot 分区
3. 配置 config.txt 使用该内核

## 生成完整镜像

如果需要包含所有模块的完整引导镜像，请运行：
```bash
./build.sh --board bbb bootstrap
```

这将生成 `bootstrap-image-bbb.elf` 和 `bootstrap-final-bbb.elf`

---
自动生成于 2026-02-04 10:24:47
