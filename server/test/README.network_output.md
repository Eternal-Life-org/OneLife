# 铜管灌溉断连：缓冲发送与后续排查

## 已确认的根因与现场验证

原逻辑对每条游戏消息执行一次非阻塞发送，返回值只要不等于消息长度就关闭
玩家连接。`log.txt` 中两次断连都由 MX 写入返回 `-2 / EAGAIN` 触发，说明 socket
当时只是暂时不可写，却被当作致命错误。部分发送也需要保留未发送尾部，不能
断连或重发整条消息。

`log.txt.2` 的同一灌溉现场启用缓冲后，玩家保持连接直至发送 `DIE 0 0` 主动退出，
没有重连。23 个完整的活跃统计窗口中，25296 条消息使用 3632 次发送系统调用，
比逐消息发送少约 85.6%。活跃窗口最大待发量 891 字节、采样最大等待约 17 ms。
主循环空转也明显减少。退出后的 `Broken pipe` 发生在 `DIE` 之后，属于客户端
关闭连接后的发送错误。

这一轮现场没有再次出现 EAGAIN，因此重试行为还通过实际 TCP 背压测试验证。
已确认并修复的是上述发送误判；其他原因造成的客户端崩溃仍需要独立证据。

## 正常发送路径

所有已登录玩家的输出始终通过 `ServerSendBuffer`，无需启用开关。
临时不可写、EINTR 保留未发字节，稍后重试；部分写入仅推进实际发送的字节数。
默认在 20 ms 窗口内合并 socket 写入，保留原始协议字节和消息顺序，包括
MX/CM/MC 的二进制压缩数据、`#` 分隔与 FM 帧结束标记。游戏逻辑消息不会丢弃，
旧客户端无需升级。客户端仍可能打印大量 MX，这不代表每条 MX 都单独调用 send。
登录前的 FreshConnection 握手及一次性查询保持原有路径。

每个连接每次排出步骤最多一次非阻塞发送、最多 64 KiB。正的亚毫秒等待向上取整
为 1 ms，避免截成 0 导致忙循环。灌溉、地图更新和合成规则没有修改。

排空、断连和重连时释放队列；`LiveObject` 在 `SimpleVector` 扩容、删除、教程迁移
时复制，不会复制整个积压缓冲。队列分配失败被捕获，按该连接失败处理。
字节超限、消息条数超限、全局队列预算不足、超时或真实 socket 错误均关闭相应
连接，继续使用既有重连流程。

代码集中在 `server.cpp` 与头文件 `ServerSendBuffer.h`，旧 Makefile 无需新增
`.cpp` 编译项。重新编译并重启测试服后生效。

## 功能参数

下列配置在启动时读取，修改后需要重启。缺失文件时使用默认值：

| settings 文件名 | 默认值 | 作用 |
| --- | ---: | --- |
| `networkSendBufferIntervalMS.ini` | 20 | 合并 socket 写入；允许 0～1000 ms，重试至少间隔 1 ms |
| `networkSendBufferMaxBytes.ini` | 1048576 | 每连接最多 1 MiB 待发协议字节；允许 64 KiB～64 MiB |
| `networkSendBufferMaxTotalBytes.ini` | 67108864 | 所有连接共用 64 MiB 队列内存预算；允许 1～256 MiB |
| `networkSendBufferMaxAgeSeconds.ini` | 15 | 最老未完成消息等待达到 15 秒时关闭该连接；允许 1～300 秒 |

另有每连接 16384 条待发消息的固定上限。全局预算包含字节容量、消息元数据、
保守估算的队列对象开销，并检查扩容时旧、新缓冲共存的峰值；它不代表进程总
内存上限。

`networkSendBufferEnabled`、`networkWriteDebug`、`networkWriteDebugInterval` 已移除。
部署目录中残留这些旧文件也不会切换发送行为。不再进行周期 MAP_UPDATE/NET_WRITE
统计、MX 计数、地图计时、正常发送时的内核查询或每 5 秒配置读取。

## 再次断连时收集什么

启动时一条 `[NET_CONFIG]` 记录实际功能参数。保留常规 INFO 日志即可
（`logLevel=4`），不需要常开逐消息 DETAIL 日志。

发送失败时保留两条低频日志：

- `[NET_WRITE_ERROR]`：玩家 ID、世界坐标、明确失败原因、消息类型、请求长度、
  返回值、原始系统错误及调用位置。
- `[NET_WRITE_ERROR_STATS]`：待发字节/消息条数、峰值、全局队列内存、最老等待时间，
  以及此连接累计的临时不可写、部分发送、EINTR 次数。内核状态仅此时查询；Linux
  包括 sndbuf、kernel_outq、unacked、retrans_total、rtt_us，不支持的项为 -1。

结合已有的 `marked as disconnected`、连接/重连、死亡/退出日志，按同一玩家 ID
和时间检查事件顺序：

| 失败原因或现象 | 排查方向 |
| --- | --- |
| `Network output queue timed out` | 客户端长时间不读或网络持续背压；比较 oldest_ms、would_block、内核队列和重传情况 |
| `Network output queue full` / `message limit` | 消息生产长期快于接收，或单条消息过大；查看待发量和消息类型 |
| `Network output global memory limit` / `allocation failed` | 发送队列预算或进程内存压力；查看 total_queue_memory 和机器内存 |
| EPIPE / ECONNRESET | 对端确实已关闭连接；同时收集客户端崩溃日志，判断是否客户端先崩溃 |
| 只有读取失败，没有 NET_WRITE_ERROR | 走的是接收/对端关闭路径；查看已有断连原因及客户端日志 |
| 服务端进程退出 | 收集退出状态、系统 OOM 日志或 core/崩溃堆栈，不能仅凭网络错误推断原因 |

临时 EAGAIN 本身不再关闭连接；持续不能发送而达到上限时才失败。内核快照只能
说明失败时的状态，不能证明客户端已处理消息，也不能单独判断客户端为何关闭。

复现时记录时间、玩家 ID、位置和操作，保留断连前后的服务端日志及客户端日志。
若这些日志仍不能归类，在测试服针对该连接短时使用 `ss -tinm` 与抓包观察 TCP
窗口/重传，并按需要加定向统计；无需把逐帧统计长期放回所有连接的发送路径。
若客户端崩溃，优先使用崩溃堆栈定位，再判断是否与网络帧处理有关。

Bot 的接收实现还有独立的已有问题：识别 MC/CM 头后，即使二进制负载已全部在
本地缓冲里，也会先等待下一次 recv。合并发送可能使停顿更明显。本次未修改 Bot；
其停顿不能直接视为服务端断连，应先确认服务端连接日志。

## 本地验证

在 `OneLife/` 执行：

```sh
python3 server/test/runServerSendBufferTests.py
python3 server/test/runServerSendBufferTests.py --sanitize
```

测试覆盖 EAGAIN、部分发送、EINTR、消息顺序、二进制内容、20 ms 合并、队列和
全局预算、消息条数、超时、重置、内存分配失败、多个连接隔离及 SimpleVector
复制。实际 TCP 测试链接 minorGems Socket，暂停对端读取产生背压后恢复，验证
字节流完全相同，并检查对端关闭后的发送不会终止进程。

兼容测试从现有 `gameSource/LivingLifePage.cpp` 提取未改写的解析函数，使用实际
minorGems 压缩代码，验证 CM/MC/MX/FM 在逐字节、7 字节分段和整批接收时，普通
与等待完整帧两种模式均能解析。它不替代旧客户端在测试服的实际游玩验证。

Mac 和 NDK Linux 头文件目标的完整 `server.cpp` C++11 语法检查用于验证编译分支；
NDK 检查不代表已在生产 Linux 服务器运行。本地验证不启动、更新或停止任何线上服。
