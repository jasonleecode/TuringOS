# L4Re Shell 和启动配置指南

## 概述

L4Re 系统有多种启动方式，从简单的单程序启动到复杂的交互式 shell。

## 启动入口类型

### 1. 直接启动 (fiasco-base-test)

**特点**: 最简单，直接运行一个程序

```
entry fiasco-base-test
roottask moe --init=rom/hello
module l4re
module hello
```

**使用场景**:
- 快速测试单个应用
- 嵌入式系统固定功能
- 最小化系统资源使用

**启动**:
```bash
ENTRY=fiasco-base-test ./build.sh --board virt bootstrap
```

### 2. Lua 配置启动 (demo)

**特点**: 使用 Lua 脚本控制启动流程

```
entry demo
roottask moe rom/demo.cfg
module l4re
module lua
module demo.cfg
module hello
```

**demo.cfg 示例**:
```lua
local L4 = require("L4");

-- 启动多个程序
L4.default_loader:start({}, "rom/hello");
L4.default_loader:start({}, "rom/another_app");

-- 保持运行
while true do
  L4.sleep(10000);
end
```

**使用场景**:
- 启动多个程序
- 动态配置系统
- 演示和测试
- 教学用途

**启动**:
```bash
ENTRY=demo ./build.sh --board virt bootstrap
./run_qemu_virt.sh
```

### 3. 完整的 ned Shell (开发中)

**特点**: 交互式 Lua REPL，带完整 L4Re API

**需求**:
- 编译 ned 包 (位于 l4re/ned/)
- 包含 lua++, libloader 等依赖
- 可选: readline 支持

**预期功能**:
```lua
-- 在 ned shell 中可以交互式执行:
> L4.default_loader:start({}, "rom/hello")
> for k,v in pairs(L4) do print(k) end
> -- 动态加载和启动程序
```

## 当前推荐配置

### 方案 A: Demo 系统 (当前默认)

```bash
# 构建
./build.sh --board virt all

# 运行
./run_qemu_virt.sh
```

系统会:
1. 显示系统信息
2. 启动 hello 演示程序
3. 持续运行并每10秒打印心跳
4. 按 Ctrl-A X 退出

### 方案 B: 自定义配置

创建你自己的 .cfg 文件：

```bash
# 1. 创建配置文件
cat > conf/mycustom.cfg << 'EOF'
local L4 = require("L4");

io.write("My Custom System Starting...\n");

-- 你的启动逻辑
L4.default_loader:start({}, "rom/hello");

-- 保持运行
while true do
  L4.sleep(1000);
end
EOF

# 2. 添加到 modules.list
# 在 l4mk/conf/modules.list 中添加:
entry mycustom
roottask moe rom/mycustom.cfg
module l4re
module[fname=ned] lua
module mycustom.cfg
module hello

# 3. 构建
ENTRY=mycustom ./build.sh --board virt bootstrap

# 4. 运行
./run_qemu_virt.sh
```

## L4Re Lua API 参考

在 .cfg 文件中可用的主要 API：

### L4.default_loader
```lua
-- 启动程序
L4.default_loader:start({}, "rom/program_name")

-- 带参数启动
L4.default_loader:start({}, "rom/program arg1 arg2")

-- 带 capabilities 启动
L4.default_loader:start({
  caps = {
    my_cap = some_capability
  }
}, "rom/program")
```

### L4.Env
```lua
-- 访问环境信息
print(L4.Env)           -- 打印环境对象
local log = L4.Env.log  -- 获取 log capability
```

### L4 工具函数
```lua
L4.sleep(milliseconds)  -- 睡眠
```

### Lua 标准库
```lua
io.write("message")     -- 输出
os.date()              -- 日期时间
string, table, math    -- 标准库
```

## 添加更多程序

### 1. 编译程序

在 `pkg/` 目录下添加你的程序，然后构建：

```bash
./build.sh --board virt l4re
```

### 2. 添加到模块列表

编辑 `l4mk/conf/modules.list`:

```
entry my-system
roottask moe rom/my-system.cfg
module l4re
module[fname=ned] lua
module my-system.cfg
module hello
module my-program      # 你的新程序
```

### 3. 在配置中使用

```lua
-- my-system.cfg
local L4 = require("L4");

L4.default_loader:start({}, "rom/my-program");
```

## 常见问题

### Q: 如何获得真正的交互式 shell？

A: 需要编译完整的 ned 包，这需要：
1. 确保 l4re/ned 源码可用
2. 配置 L4Re 构建包含 ned
3. 可能需要 readline 库支持

当前 lua 二进制是标准 Lua，缺少 L4 扩展模块。

### Q: 为什么 require("L4") 失败？

A: 标准 lua 解释器不包含 L4 模块。需要 ned (L4Re 专用的 Lua)。

### Q: 如何在 QEMU 中输入命令？

A: 当前配置是自动启动模式。真正的交互式 shell 需要 ned 的 REPL 功能。

### Q: 可以运行多个程序吗？

A: 可以！在 .cfg 文件中多次调用 `L4.default_loader:start()`。

## 下一步计划

1. **短期**: 使用 Lua 配置文件实现灵活的系统启动
2. **中期**: 编译 ned 实现基本的脚本化管理
3. **长期**: 完整的 ned REPL，实现交互式 shell

## 参考资源

- L4Re 文档: https://l4re.org/
- modules.list 格式: `l4mk/conf/modules.list` 中的注释
- 配置示例: `l4mk/conf/examples/*.cfg`
