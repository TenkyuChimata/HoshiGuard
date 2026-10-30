# 可选接收诊断与证据工具

0.3.0 的生产入口是 `hoshiguard --port /dev/hoshiguard --api`。
旧 CLI 的详细观察、证据审计和 Human 输出迁入 `hoshiguard diagnose`，不在生产 API
进程中创建统计文件、session 元数据或重复输出。此处保留用于调试和复核的能力，
不要求长期运行部署同时启用它们。

```sh
hoshiguard diagnose --help
hoshiguard diagnose --bridge-file session.hgraw --human
hoshiguard diagnose --bridge-file session.hgraw --semantic-observations --ndjson alerts.ndjson
hoshiguard diagnose --bridge-file session.hgraw --all-observations --ndjson observations.ndjson --json-report counts.json
hoshiguard diagnose --capture-log legacy-capture.log --human
```

模式说明：

- all-observations：每个通过 Python 验证的帧生成观察记录，用于证据核算。
- semantic-observations：同一 semantic fingerprint v2 仅输出一次（诊断入口默认）。
- events-only：输出明确的业务 action；不把 SLAS/Null/保留 MT 当 issued。
- repeat-summary：回放末尾输出有界聚合，统计卫星/PAB/帧证据，不是额外灾害事件。

Human 和详细 NDJSON 共用 UnifiedAlert，没有第二套解码；NDJSON 可以包含原始 UBX/L1S
以及内部路径和 session 资料，不适合直接当作公开 API。`--omit-raw-json` 可隐藏 raw，
`--no-session-records` 可省略 session_start/end，供已有证据消费者使用。
这些诊断格式和 HGRAW 1.x 没有迁移。旧命令只需在 hoshiguard 后加 diagnose。

`--output-directory`、`--session-id`、`--rotate-size`、`--rotate-interval`、
`--fsync-interval`、`--health-file`、`--pid-file` 和磁盘策略参数仅属于诊断入口。
生产进程使用 systemd 管理 PID，以内存 status 替代 health 文件。不要让诊断程序和
生产程序同时读取同一个串口；应分析生产留下的 HGRAW。

## 工具分类

| 工具 | 用途 |
|---|---|
| analyze_capture / analyze_semantic_identity | 离线捕获与语义身份分析 |
| validate_bridge_session / analyze_soak_session / audit_ndjson_stream | 会话完整性、长期运行证据和输出核算 |
| import_real_fixtures / import_bridge_session_fixture / import_24h_session_fixture | 从真实证据提取可公开 fixture |
| validate_dcr_test_scenario / validate_dcx_test_scenario | 按官方计划核对实际收到的场景 |
| sanitize_capture / capture_io / replay_ubx | 隐私清理、采集和受控硬件注入 |
| check_spec_inventory / check_alert_support_matrix / check_alert_traceability | 规范、实现和证据对应检查 |
| benchmark_bridge / benchmark_api / run_native_sanitizers | 可复现软件性能和原生验证 |

完整现场捕获不在仓库中的测试仍会 skip，不能将缺少原始现场数据解释为验证通过。
[支持矩阵](support_matrix.md)、[证据出处](evidence_provenance.md)、
[Bridge 协议](bridge_protocol.md) 和各月官方测试计划保留，作为独立技术资料。

## MCU 诊断

`platformio.ini` 保留显式命名的 debug、capture、capture-l1s、loopback、
bridge-text、json 等环境（以配置文件实际环境为准）。默认环境改为 bridge-binary；
独立显示与文字诊断由预处理分支和 LTO 排除在 Binary Bridge 运行热路径之外。
本次没有为了删文件重写 GNSS 配置、自检、PPS、UART 诊断或 MCU 解码器，避免失去
现场异常恢复与独立验证能力。
