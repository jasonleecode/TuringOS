# 原生应用的 POSIX 接口

`app.mk` 为 L4Re 程序链接 `task_io` 和 `ext4_client`，并保留两者的初始化函数。系统在启动进程时决定它能访问的服务。下表适用于 QEMU virt 的 `native-shell` 镜像和通过 `app run` 启动的 ELF。

| 用途 | 应用接口 | 安装权限及行为 |
| --- | --- | --- |
| 标准输入输出 | `stdin`、`stdout`、`read`、`write` | 无需可选权限。应用可以作为 shell 管道的输入端或输出端。重定向的目标由 shell 打开。 |
| 持久文件 | `fopen`、`fread`、`fwrite`、`fclose` 等 | 授予 `data` 后可读写 `/data`，同一文件在系统 shell 中位于 `/ext4/data` 下。获得此权限的应用共享该目录。 |
| 临时文件 | 与普通文件相同 | 授予 `tmp` 后可访问共享的 `/tmp`，重启后内容清空。 |
| TCP 客户端 | `socket`、`connect`、`send`、`recv`、`read`、`write`、`close` | 授予 `net` 并启用 netd。支持阻塞的 `AF_INET` / `SOCK_STREAM` 客户端。一次收发可能只处理部分数据，调用者按返回值继续传输。 |

省略可选权限时，应用仍可通过标准输出写日志或把结果交给管道。需要直接创建文件时，使用已挂载的应用目录，例如 `/data/message`。当前 socket 适配器没有提供 UDP 或服务端的 `listen` / `accept`。WASM 模块可用的接口由 WAMR 的导入表决定。

[管道与标准流](../../spawn/io/pipe.cc) · [TCP 适配器](../../spawn/io/socket.cc) · [应用权限处理](../../native_shell/tools/app.cc)
