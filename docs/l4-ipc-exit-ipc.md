# L4 IPC 退出通知：知识点汇总

本文记录在实现 spawnd 子进程退出检测时踩到的 L4 IPC 坑，以及最终正确的架构设计。

---

## 1. IPC gate 的挂起发送者语义

**现象**：子进程调用 `l4_ipc_call(parent_gate, L4_IPC_NEVER)` 时，若 gate 尚未绑定接收线程，调用**不会立即失败**，而是阻塞，挂起在 gate 的发送者队列里。

**后果**：若 `do_wait` 用轮询 `l4_thread_stats_time` 判断子线程是否存活，会发现线程一直活着（它在等 IPC 回复），导致无限循环。

**正确认知**：只有绑定了接收线程的 gate 才能接收 IPC。绑定之前，所有发送方都会挂起等待。

---

## 2. KR_REPLY 槽与服务器分发线程

`Registry_server::loop()` 分发 IPC 时，内核把应答 cap 存入当前线程的 `KR_REPLY` 槽。

**约束**：在 `op_spawn` / `op_wait` 调用栈内，**任何 `l4_ipc_receive` / `l4_ipc_reply_and_wait`** 都会覆盖 `KR_REPLY`，导致后续无法回复最初的 spawnd 客户端。

因此，等待子进程退出的 IPC 接收**必须在独立线程（reaper）中完成**，不能在服务器分发线程里做。

---

## 3. IPC gate 标签的低 2 位限制

`l4_ipc_gate_bind_thread` / `l4_rcv_ep_bind_thread_u` 要求：

> 标签（label）的最低 2 位必须为零。

内核在投递时会把能力权限位 OR 进这两位。若传入的 label 低位非零（如 `h = 1`），调用返回 `-L4_EINVAL`（-22）。

**正确做法**：绑定时左移 2 位：`label = (l4_umword_t)handle << 2`；接收时右移 2 位：`handle = label >> 2`。

---

## 4. 必须在 launch 之前绑定 gate

`loader.launch()` 启动子线程后，子进程可能在 `do_spawn` 返回之前就调用 `l4_ipc_call(parent)` 退出（特别是 `return 0` 型短命进程）。

**顺序要求**：
```
l4_ipc_gate_bind_thread(gate, reaper_cap, label)   ← 必须先
loader.launch(...)                                  ← 再启动子进程
```

反之，若先 launch 再 bind，子进程的 exit IPC 会挂起在无人接收的 gate 上，`do_wait` 永远等不到信号。

---

## 5. LOADING 状态竞争

`do_spawn` 在持锁下把槽设为 `LOADING`，然后释放锁做 ELF 加载。子进程可能在 `do_spawn` 把槽转为 `RUNNING` 之前就已退出。

**两处修复**：
1. reaper 接收退出 IPC 时，只要 `state != EXITED` 就标记为 EXITED（不判断是否 RUNNING）
2. `do_spawn` 在重新持锁填充 caps 后，仅在 `state == LOADING` 时才转为 RUNNING，以免覆盖已经被 reaper 写入的 EXITED 状态：

```cpp
if (slot->state == Child_task::LOADING)
    slot->state = Child_task::RUNNING;
```

---

## 6. 正确的 reaper 线程模式

专用 reaper pthread，持有自己的 UTCB/KR_REPLY，不影响服务器分发线程：

```
reaper_main:
  l4_ipc_wait(...)          ← 初始开放等待
  loop:
    if no error:
      label >> 2  → handle
      mr[0/1]     → exit_code
      标记 EXITED
      l4_ipc_reply_and_wait(...)  ← 回复子进程，再次等待
    else if RETIMEOUT:
      reap_once()            ← 用 l4_thread_stats_time 检测崩溃
      l4_ipc_wait(...)
```

**关键点**：
- `l4_ipc_reply_and_wait` 原子地发送回复并开始下一次等待，是标准服务器循环写法
- `do_wait` 只轮询 `t->state`（10 ms sleep），完全不碰 IPC，KR_REPLY 槽安全
- `pthread_l4_cap(thread)` 在 `pthread_create` 返回后立即有效，可直接传给 `l4_ipc_gate_bind_thread`

---

## 7. 调试辅助

| 现象 | 原因 | 检查点 |
|------|------|--------|
| `do_wait` 永远不退出，`l4_thread_stats_time` 返回成功 | gate 未绑定，子进程挂起等回复 | 确认 bind 在 launch 之前 |
| `l4_ipc_gate_bind_thread` 返回 -22 | label 低 2 位非零 | 改用 `handle << 2` |
| reaper 收到 EXITED 但 `do_wait` 还在轮询 | LOADING 状态竞争，reaper 跳过了该槽 | reaper 改为 `state != EXITED` 判断 |
| 退出码为 0 但实际非零 | `mr[0]` 是 label，`mr[1]` 才是 exit_code | 检查 `l4_msgtag_words` 后再读 mr |
