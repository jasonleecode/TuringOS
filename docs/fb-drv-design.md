# fb-drv 设计文档

## 1. 背景与定位

### 1.1 L4Re 显示栈现状

当前 TuringOS 的显示路径非常简单：

```
QEMU ramfb (物理帧缓冲)
    │ sigma0 映射
    ▼
  MOE：读取 bootstrap MBI 中的 VBE 信息，
        映射物理帧缓冲，注册 "vesa" Goos cap
    │ ned 显式传递
    ▼
  fb-test (直接读写物理帧缓冲内存)
```

这条路径的问题：
- 应用直接持有物理内存的 dataspace，没有隔离
- 只支持单一客户端（谁拿到 vesa cap 谁写屏）
- 与硬件强耦合：换成真实 HDMI 硬件就要改所有客户端

### 1.2 fb-drv 的定位

`fb-drv` 是**用户态帧缓冲驱动服务器**，在物理帧缓冲（MOE vesa cap 或真实硬件）和应用程序之间增加一个隔离层，实现标准的 `L4Re::Video::Goos` IPC 服务。

引入 fb-drv 后的架构：

```
QEMU ramfb / RPi4 HDMI 硬件
    │
    ▼
  MOE "vesa" cap（QEMU 路径）
  或 io/vbus 设备树（真实硬件路径）
    │ ned 传递给 fb-drv
    ▼
┌─────────────────────────────────┐
│  fb-drv (用户态 Goos 服务器)    │
│  - 持有物理帧缓冲 dataspace     │
│  - 实现 L4Re::Video::Goos IPC  │
│  - 支持多客户端（未来）          │
└─────────────────────────────────┘
    │ Goos IPC (标准协议)
    ▼
  fb-test / GUI app / 窗口管理器
```

---

## 2. L4Re Goos 协议

### 2.1 核心接口

`L4Re::Video::Goos` 是 L4Re 的显示设备抽象接口（类似 DRM/KFB），定义在：
- `l4re/include/video/goos`
- `l4re/util/include/video/goos_svr`（服务端 Mixin 基类）
- `l4re/util/include/video/goos_fb`（客户端封装）

主要 IPC 操作：

| 操作 | 描述 |
|------|------|
| `op_info` | 返回屏幕信息（宽、高、像素格式、flags） |
| `op_view_info` | 返回 View 信息（位置、尺寸、buffer 偏移） |
| `op_get_static_buffer` | 返回帧缓冲 dataspace（客户端映射后直接写像素） |
| `op_refresh` | 通知驱动刷新指定区域（auto-refresh 时为 no-op） |
| `op_set_view_info` | 设置 View 参数（未来支持多 View） |
| `op_create_view` | 创建新 View（多客户端窗口，未来） |

### 2.2 现有实现参考

MOE 中的 `Vesa_fb`（`l4re/moe/server/src/vesa_fb.cc`）是最直接的参考：
- 继承 `L4Re::Util::Video::Goos_svr`（处理 IPC dispatch）
- 继承 `L4::Epiface_t<..., L4Re::Video::Goos>`（注册到 IPC 系统）
- 用 `Dataspace_static` 包装物理帧缓冲内存

fb-drv 的结构与之基本相同，区别是：
- MOE 的 Vesa_fb 运行在 root task 中，直接用 sigma0 映射物理内存
- fb-drv 运行在普通用户任务中，从 vesa cap 获取已映射好的 dataspace

---

## 3. 阶段规划

### 第一阶段：Goos 代理服务器（当前目标）

**功能**：接管 MOE 的 vesa cap，对外暴露标准 Goos IPC 服务。

**实现要点**：
1. 启动时从环境获取 `vesa` capability（MOE 的原始帧缓冲）
2. 通过 `Goos::info()`、`Goos::get_static_buffer()` 读取屏幕参数和 dataspace
3. 用这些信息初始化自己的 `Goos_svr`
4. 在 `fb` capability（server 端）上循环处理 IPC 请求
5. `refresh()` 在 ramfb 模式下为 no-op（ramfb 自动刷新）

**ned 配置**：
```lua
-- conf/fb-drv.cfg
local L4 = require("L4")
local l   = L4.default_loader

local vesa   = L4.Env.vesa       -- MOE 注册的原始帧缓冲
local fb_svr = l:new_channel()   -- fb-drv 对外暴露的 Goos 接口

l:start({
  log  = { "fb-drv", "blue" },
  caps = { vesa = vesa, fb = fb_svr:svr() },
}, "rom/fb-drv")

-- fb-test 不直接访问 vesa，改为访问 fb-drv
l:start({
  log  = { "fb-test", "green" },
  caps = { vesa = fb_svr:m("rws") },
}, "rom/fb-test")
```

**关键数据流**：
```
fb-drv 启动
  → gfb_in.init("vesa")          -- 连接原始帧缓冲
  → gfb_in.view_info()           -- 读取 width/height/bpp/bpl
  → gfb_in.attach_buffer()       -- 映射帧缓冲 dataspace 到自己地址空间
  → 用这些信息填充 Goos_svr 结构体
  → 在 "fb" cap 上 server.loop() -- 开始服务 IPC

client 请求 get_static_buffer
  → fb-drv 把自己映射的同一个 dataspace 返回给 client
  → client 映射后直接写像素（零拷贝）
```

**文件结构**：
```
pkg/fb-drv/
  Control
  Makefile
  server/
    Makefile
    src/
      Makefile
      main.cc
```

### 第二阶段：多客户端与合成（未来）

- 每个客户端分配独立的 off-screen buffer（virtual framebuffer）
- fb-drv 定期或按 refresh 请求将各客户端的 buffer 合成到物理帧缓冲
- 支持 `op_create_view` / `op_set_view_info` 控制窗口位置和层叠
- 这是轻量级窗口管理器的基础

### 第三阶段：真实硬件支持（RPi4 HDMI）

RPi4 的 HDMI 路径与 QEMU ramfb 不同：
- 硬件通过 VideoCore GPU 管理，BCM2711 的 HDMI 控制器通过 mailbox 协议配置
- bootstrap 在真实 RPi4 上同样会填充 VBE 信息（如果 firmware 支持），但更可靠的方式是通过 mailbox 直接查询帧缓冲地址
- fb-drv 未来需要通过 `vbus` 访问 BCM2835 mailbox 驱动，获取帧缓冲参数
- 对上层接口（Goos IPC）保持不变，切换硬件路径对 GUI 应用透明

---

## 4. 与现有组件的关系

| 组件 | 职责 | 与 fb-drv 关系 |
|------|------|---------------|
| MOE vesa cap | 映射物理帧缓冲，注册 Goos cap | fb-drv 的输入（QEMU 路径） |
| fb-test | 绘制测试图案，验证帧缓冲 | 重构为 fb-drv 的 client |
| fb-drv | 用户态 Goos 服务器 | 本文档主题 |
| GUI app（未来）| 窗口、控件渲染 | fb-drv 的 client |
| 窗口管理器（未来）| 布局、焦点管理 | 基于 fb-drv 第二阶段多 View 能力 |

---

## 5. 依赖与构建

**REQUIRES_LIBS**：
```
stdlibs compiler-rt libstdc++ l4re l4re_c l4re-util l4util
```

**编译验证**：
```bash
make -C build/l4re_virt pkg/fb-drv
```

**运行验证**：
```bash
# 构建 bootstrap 镜像
make -C build/l4re_virt E=fb-drv elfimage \
  MODULE_SEARCH_PATH="build/kernel_virt:build/l4re_virt/bin/arm_armv7a/l4f:build/l4re_virt/lib/arm_armv7a/std/l4f:conf"

# 启动（GTK 窗口直接显示）
./run_qemu_virt.sh --gpu
```

---

## 6. 遗留问题

1. **多 client 权限**：当前 `get_static_buffer` 直接返回物理帧缓冲 ds，多个 client 会互相覆盖，第二阶段需要 per-client virtual buffer + 合成。
2. **refresh 通知**：ramfb 是 auto-refresh，但真实 HDMI 可能需要 vsync 同步，第三阶段再处理。
3. **像素格式协商**：当前 client 必须与硬件格式一致（32bpp XRGB8888），未来 fb-drv 可以做格式转换。
