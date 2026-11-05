# 登录失败原因协议

游戏服统一发送 `REJECTED\nERR1 <CODE>\n#`。首行及 `#` 分帧规则与旧版相同，
旧客户端和 OneLifeBot 仍识别为登录拒绝；新客户端只解析已知的固定码，不展示服务端正文。
旧服的 `REJECTED\n#`、未知码和畸形详情按客户端原有登录阶段提示降级。
`ACCEPTED`、`SERVER_FULL`、`SHUTDOWN`、`NO_LIFE_TOKENS` 格式不变。

| 公开码 | 含义 |
| --- | --- |
| `AUTH_INVALID` | QQ 号或密钥不匹配 |
| `AUTH_BLOCKED` | QQ 号和密钥均验证正确，但账号被封禁 |
| `WHITELIST_DENIED` | 验票通过，但不符合当前服务器白名单 |
| `AUTH_SERVICE_UNAVAILABLE` | 验票超时、HTTP/正文异常或请求数达到上限 |
| `CLIENT_INCOMPATIBLE` | LOGIN 格式、散列格式或服务器密码不兼容 |
| `LOGIN_TIMEOUT` | 服务器等待登录请求超时 |
| `FAMILY_UNAVAILABLE` | 指定家族不存在或没有可生育成员 |
| `LIVE_UNAVAILABLE` | 观看目标已离线、停止直播或不可观看 |
| `JOIN_UNAVAILABLE` | 通过验证后无法进入游戏的其他情况 |
| `SERVER_ERROR` | 登录处理的其他错误 |

## 验票与资源边界

游戏服调用 `check_ticket_hash&reason_version=1`。OneLifeWeb 的 PHP 8.4 接口仅在
QQ 号及密钥 HMAC 正确时返回 `INVALID\nACCOUNT_BLOCKED`；未选择新版本的请求
仍返回 `VALID` 或 `INVALID`。错误 QQ 号与错误密钥均返回 `INVALID`。
成功更新游玩记录后才返回 `VALID`，本次不更改数据库表结构。

游戏服仅接受 HTTP 200 和上述三种精确正文。HTML、500、额外文本、空值及包含
`VALID` 的异常正文均按验证服务故障拒绝。HTTP 响应（含头部）最多 4 KiB，
超时保留 8 秒。活动和待回收验票请求合计最多 64 个；超时 DNS 请求交给后台回收，
不在游戏主循环析构。登录前接收缓冲上限为 2 KiB，数值解析不抛出转换异常。

每个连接只保留首次拒绝原因。拒绝包按非阻塞发送进度续发，最多等待 5 秒后关闭。
新增拒绝日志格式为 `[LOGIN_REJECT] connection=<关联号> phase=<阶段> code=<公开码>`。
白名单仍只在启用 TicketServer 验证时检查，空配置沿用原有放行规则。

## 构建与发布

OneLife 依赖此次 minorGems 的 `WebRequest` 状态码和响应长度接口，构建游戏服前应
同时更新两个仓库并重新编译。按 OneLifeWeb（PHP 8.4）→ 测试游戏服 → 正式游戏服
夜间维护 → 客户端的顺序发布。观察登录成功率和 `[LOGIN_REJECT]` 的码分布。
旧客户端仍显示笼统拒绝，可通过服务端日志确认实际原因。

Web、游戏服或客户端单独回滚仍兼容旧协议，部分细分原因会降级。

在各仓库根目录执行本次回归：

```sh
# OneLifeWeb：隔离 Docker HTTP/MySQL 环境，不使用生产库
python3 tests/php84_integration.py
# minorGems：本地 TCP HTTP 状态码、长度限制和旧调用默认值
python3 network/web/test/runWebRequestTests.py
# OneLife：登录解析、拒绝包、后台回收和旧客户端解析
python3 server/test/runLoginFailureTests.py
python3 server/test/runServerSendBufferTests.py
```
