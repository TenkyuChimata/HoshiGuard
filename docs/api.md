# HTTP / WebSocket API v1

本进程内的只读接口。默认 `127.0.0.1:8000`。数据来源是独立通过 Bridge、UBX、
L1S 校验并由唯一解码链生成的 UnifiedAlert。HTTP 不扫描文件，WS 不重复解码。

## HTTP

响应使用 `Content-Type: application/json`、`Cache-Control: no-store`；当前不实现
ETag。方法不允许时 405，参数非法时 400，不存在时 404，限流时 429，连接达到上限
时 WS 握手返回 503。应用错误只返回稳定错误标识，异常详情在本机日志。

| 路径 | data |
|---|---|
| `/api/v1/alerts/latest` | 最新匹配的 PublicAlert，无数据时 `null` |
| `/api/v1/alerts` | 最新在前的 PublicAlert 数组，无数据时 `[]` |
| `/api/v1/alerts/{id}` | 缓存中的单条语义消息；过期/淘汰后 404、`data:null` |
| `/api/v1/status` | 直接返回健康对象 |

alerts 响应信封：

```json
{"schema":"hoshiguard.api.v1","process_id":"进程UUID","sequence":17,"data":null,"next_offset":null,"status":{}}
```

以上为**格式示意**；完整真实数据见 [Marine](examples/public-alert-real-marine.json) 和
[DCX](examples/public-alert-real-dcx.json)，出处见 [示例说明](examples/README.md)。

`/alerts` 与 `/alerts/latest` 参数：

| 参数 | 取值与默认 |
|---|---|
| `service` | `DCR` / `DCX`；省略表示两者 |
| `category` | 响应中 `category.token` 的精确值，例如 `MARINE` / `MISSILE_ATTACK` |
| `status` | 默认 `actual`；可选 `test`、`unknown`、`all` |
| `limit` | 1–100，默认 100；latest 总是 1 |
| `offset` | 0–history-limit，默认 0 |
| `view` | 默认 `history`；`states` 返回各可保守关联对象的最近状态 |

不接受重复或未知参数。每个列表响应最多包含 1MiB 的通报 JSON；按实际返回数
增加 offset，直至 data 为空。`next_offset` 指向下一次查询偏移，末尾可能还需要一次
空请求。列表是滚动内存窗口，分页期间若 `sequence` 改变，应重新获取，不能将多次
请求冒充同一事务。单个响应的 data、sequence、status 在同一状态锁内取样。

by-id 查询不按 status 过滤：已经持有训练消息 ID 的客户端可以读取它，但正式默认
列表和正式 WS 通道不会出现训练消息。接口无认证，不提供管理、写入或调试功能。

## PublicAlert

Schema 为 [`hoshiguard.public-alert.v1`](schema/public-alert.v1.schema.json)。
内部详细格式 `hoshiguard.alert.v1` 保留给诊断与证据分析。v1扩展可增加字段，
客户端应忽略未知字段；既有字段的类型和含义若发生破坏性改变需新major版本。

| 字段 | 含义 |
|---|---|
| `schema` | 公开模型版本 |
| `id` | `sf2-` 加完整 semantic_fingerprint v2；作用域是语义消息内容 |
| `source` | QZSS 卫星广播来源；JMA 或 CAMF country/provider 原始代码及已知名称 |
| `service` | DCR / DCX |
| `category` | 原始代码、token、known/reserved 和已有的人类可读名称 |
| `status` | actual / test / unknown；DCR Rc=7、DCX A1=0 及明确的13/113试验灾害类别归test |
| `action` | issue / update / cancel / clear / unknown |
| `identity` | semantic fingerprint 版本、`event_id:null`、有限可信度关联线索 |
| `times` | issued/received/onset/expires、时间依据与必要原始时间码 |
| `areas` | 通报目标区域代码、名称、kind；不是设备坐标 |
| `severity` | 规范携带的等级/警报原码及说明；未提供时 null |
| `payload` | 按灾害类别命名的 type 与该类别字段；不制造缺失震源参数 |
| `validity` | 传输验证、语义、完整性、类别是否已知及未知字段 |
| `is_real_disaster` | 正式且已知的灾害业务通报；训练、试验灾害类别不为 true |
| `provenance` | 规范、live/replay 来源；明确不是 JMA XML 原文 |

DCR `payload.type` 包括 earthquake_early_warning、hypocenter、seismic_intensity、
nankai_trough_earthquake、tsunami、northwest_pacific_tsunami、volcano、ash_fall、
weather、flood、typhoon、marine；`payload.fields` 保留现有类别解码字段与 `_raw` 值。
信息类型1的更正在公开action中表示update，原始It=1和Correction标签仍保留。
EEW的assumptive标记为true时，固定De/Ma=10不作为真实深度/震级，公开值为null，
原码与measurement_basis保留。

DCX `payload.type` 来自 CAMF hazard token，例如 missile_attack、earthquake、flood。
`camf` 保存 A1–A18 原码，`guidance` 提供已解码的指示文字，`extended` 保存对应变体
及字段。避难指示依赖 provider、A11 和 EX，应读取 guidance，不能只靠 hazard 或 A1。
明确的siren/test类别即使A1=Alert也进入公开test域，原始A1与内部身份仍保留。
J-Alert的撤销避难呼吁可能仍是A1=Alert；不会将该文本擅自转成整个灾害的 clear。
EX9 同时提供固定16位十六进制字符串 `EX9_target_area_code_list_hex`，避免 JavaScript
等客户端将64bit JSON整数舍入；原始数值仍保留。EX11只保留原始分段值并标记不完整；
未知库版本不解释其A11文字。

未知/保留值不填 0 或升级成已知值。未支持的业务头不会作为有效公开通报发布；原始
Bridge 帧仍可归档。已知核心的部分解码可以发布 `complete:false`，客户端必须检查。
`is_real_disaster` 不表示警报仍在生效，也不是消息真实性的密码学认证。

## 时间与标识

DCR 月/日/时/分是 UTC，报文不携带年份。解码使用 PC 接收时间作为年份上下文；
HGRAW 回放使用文件中的原始 PC 时间。ISO 8601 输出 `+00:00`，JST 显示时加 9 小时，
不要再把广播原码当 JST 减 9 小时。所有 DCR 派生日期均标明年份是推算值。

DCX A6/A7 的当前/下一周及分钟原码保留。公开层目前保守地不填绝对 onset/expires，
不以接收时钟制造发布日期或凭 TTL 生成 clear。缓存过期只是本机资源管理。

`id` 不是灾害事件 ID：年份缺失/相对周循环意味着跨年相同内容可能具有相同语义 ID。
`event_key` 作为 `correlation_hint` 暴露，当前 confidence=limited，不用于自动合并。
同一消息的卫星复制只保留一次；不同更正、取消、解除各有内容 ID。历史保存这些动作，
而不会宣称已证明它们属于某个全球唯一事件。`states` 视图也因此可能包含同一灾害的
多条消息，适用于缺乏可靠关联证据的保守展示。

## WebSocket

`WS /api/v1/ws`；唯一查询参数 `status=actual|test|unknown|all`，默认 actual。
事件信封：

```json
{"type":"alert","process_id":"进程UUID","sequence":18,"data":{}}
```

这是格式示意。三个传输类型：

- `snapshot`：`data.alerts` 为最近可查询状态，最多 100 条/1MiB，
  `data.truncated` 表示截断，`data.status` 是同一时点的健康状态。
- `alert`：data 是新提交的 PublicAlert，含明确的 action。
- `status`：接收连接、有效 L1S 状态或错误计数改变时的健康对象。

客户端注册和快照在同一锁内完成，先发送 snapshot，之后发送其后的提交，避免注册
窗口丢消息。所有订阅共享已编码字符串；单客户端队列 32 条，全局最多 64 客户端。
接收线程只提交内存并发出合并唤醒，不等待网络。慢客户端队列满会清空该队列并以
1013 关闭；单次发送超过 5 秒中止连接；单个客户端失败不影响其他订阅或接收。
异常客户端以1011关闭且不泄露异常正文；关闭drain和握手合计等待最多2秒。
服务器关闭使用1001。只允许服务端业务数据，客户端发业务 TEXT/BINARY 会以 1008 关闭。
协议 ping/pong 由 aiohttp 处理，heartbeat 30s；静默时不反复发送 alert JSON。

恢复：退避重连，以新 snapshot 替换本地视图，检查 process_id；若 truncated，可用
`/alerts?view=states` 分页补取并检查 sequence，期间继续处理 WS 消息。不提供完整事件
日志或基于旧 sequence 的重传。需要完整证据时使用 HGRAW；恢复范围受缓存数量、TTL、
进程重启影响。sequence 是进程提交序号，包含 status，因此业务序号可以有间隔。

## 健康与运行边界

`receiver` 表示 starting/waiting_hello/connected/disconnected/stopped/failed/
replay/replay_complete。`link` 是接收连接状态，`signal` 区分 receiving、waiting_l1s、
no_l1s、unknown、historical。30s 未接收有效 L1S 不能证明天线完全无卫星。
`data_mode=live` 仅表示连接有近期有效 L1S，不表示某条缓存灾害消息是刚发布的。
消息时间请读 times，历史查询即使链路正常也可能返回旧通报。

15s 无 Bridge 帧标为 stalled；30s 无有效 L1S 且 Bridge 仍有帧标为 no_l1s。
单纯没有灾害新报不影响正常接收状态，SLAS/Null 仍更新有效接收时间。
断开时保留缓存但标为 historical。`decode_errors`、`verification_errors`、
`bridge_errors`、`sequence_gaps`、`upstream_errors`、`upstream_drops`、`upstream_l1s_drops`、`archive_errors`
是进程累计观测；last_decode_error 只提供错误类型代码，不泄露异常正文或路径。
节选 HGRAW fixture 本身有序号跳跃，不应据此推断完整现场链路丢帧。

历史默认 512 条/24h，去重默认 4096 条/7d，均在内存中有界。历史 TTL 从发布提交计，
重复广播不刷新原通报的发布时间或缓存保留时间；超出窗口可能没有可查询历史，但
接收健康仍独立判断。时钟 TTL 使用主机 monotonic，不受 MCU 重启和墙钟回拨影响。

默认同源浏览器可用；跨域需要 `--api-origin` 显式允许，不支持 `*`。无 Origin 的普通
客户端可读公开接口。默认全局 100 请求/秒、200 burst，无 IP 无界统计表。
反向代理应另设每地址连接/请求限制、TLS 和 header timeout；网络完全不可信时不要
直接暴露应用端口。应用不实现 TLS、登录、管理接口或原始证据下载。
