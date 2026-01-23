-- 文件路径: conf/system.ned
-- 描述: L4Re 启动脚本，负责构建系统拓扑结构

local L4 = require("L4");

-- 1. 创建一条名为 "console_channel" 的 IPC 通道
local console_channel = L4.default_loader:new_channel();

-- 2. 启动底层服务：控制台驱动 (Console Server)
L4.default_loader:start(
  {
    caps = {
      -- 给予驱动这个通道的“服务端权限(svr)”
      -- 驱动可以通过这个权限监听并处理来自通道的请求
      console_svc = console_channel:svr(),
      
      -- 给予驱动访问硬件的权限 (ICU=中断控制器, IO=内存映射外设)
      -- 只有拥有这些权限，驱动才能操作树莓派的 UART 寄存器
      icu = L4.Env.icu,
      io  = L4.Env.io,
    },
    -- 设置日志前缀为绿色的 "DRIVER"
    log = { "DRIVER", "green" } 
  }, 
  "rom/console_srv" -- 启动的 ELF 二进制文件名
);


-- 3. 启动用户应用：你的 Shell (Native Shell)
L4.default_loader:start(
  {
    caps = {
      -- 给予 Shell 这个通道的“客户端权限”
      -- Shell 把这个权限命名为 "my_console"
      my_console = console_channel,
    },
    -- 设置日志前缀为青色的 "SHELL"
    log = { "SHELL", "cyan" }
  }, 
  "rom/native_shell" -- 启动的 ELF 二进制文件名
);
