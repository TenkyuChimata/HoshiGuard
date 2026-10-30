# 示例出处

这些 JSON 由当前代码直接解码仓库 HGRAW 生成；不是将人工参数拼装成的广播。
文件中的 `input=replay` 描述此次读取方式，`times.received` 仍是现场 PC 时间。

| JSON | 归档 | 记录 offset |
|---|---|---:|
| public-alert-real-marine.json | test/fixtures/bridge_real_session/selected_records.hgraw | 220 |
| public-alert-real-dcx.json | test/fixtures/bridge_24h_session/selected_records.hgraw | 4608 |

归档 SHA-256 分别为：

- `74b27282f1438cc8e4080f771b36e93ed8bc635cb01d65a632220dcf9575eaa9`
- `e5cb12abb3295f981bbc39557a3dbd670eabff51d9eacf341e1cbdc19cdb49ed`

Marine 是真实接收的 DCR14 海上浓雾警报；DCX 是真实接收的 L-Alert 训练/测试消息，
不能用作已接收正式导弹警报或实际避难事件的证据。完整 fixture 导入与证据说明在对应
目录 README 和 docs/evidence_provenance.md。更新解码后应从同一记录重新导出示例。
