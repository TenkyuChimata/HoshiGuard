# Debian/Linux 部署

需要 Python 3.10+、`python3-venv`、可读写串口的 `dialout` 组，以及已经运行
Bridge Binary 固件的 STM32。推荐 Debian 12/13。主进程仅一个串口读取者。

以下由管理员按实际路径执行；本次重构没有实际部署：

```sh
sudo apt install python3-venv
sudo useradd --system --home-dir /var/lib/hoshiguard --groups dialout hoshiguard
# 将仓库放在 /opt/hoshiguard，确保服务用户可读。
python3 -m venv /opt/hoshiguard/.venv
/opt/hoshiguard/.venv/bin/pip install '/opt/hoshiguard/python[api]'
sudo install -d /etc/hoshiguard
sudo install -m 640 -o root -g hoshiguard packaging/systemd/hoshiguard.env.example /etc/hoshiguard/hoshiguard.env
sudo install -m 644 packaging/systemd/hoshiguard.service /etc/systemd/system/
# 编辑 env，使 HOSHIGUARD_PORT 指向实际设备；建议用 udev 建立稳定 symlink。
sudo systemctl daemon-reload
sudo systemctl enable --now hoshiguard
curl http://127.0.0.1:8000/api/v1/status
journalctl -u hoshiguard -f
```

默认不产生磁盘业务日志、session/health/PID 文件。systemd 直接管理 PID，
健康状态读 `/api/v1/status`。服务启动时串口缺失会持续重连；无需绑定设备单元
阻止服务启动。串口错误和解析错误写 journal，公开状态不返回设备路径或异常正文。

需要证据保存时，用 `systemctl edit hoshiguard` 覆盖 `ExecStart`，在原命令后增加
`--raw-archive /var/lib/hoshiguard/evidence.hgraw`。默认每 256MiB 或 24h 按需分段，
30s fsync，关闭/轮换时 fsync 并写 SHA-256 sidecar。该间隔由写入驱动，并不是
空闲时每 30 秒强制刷盘。磁盘低于 64MiB 预留时退出失败，不静默丢弃归档；
旧分段不自动删除，应由管理员安排保留期。突然断电可能丢失最后的缓冲写入，
HGRAW reader 会验证并跳过损坏/截断记录。

如需调整历史容量，在 ExecStart 增加 `--history-limit 256`。固定的内存界限、
24h 历史 TTL、4096 条/7d 语义去重适用于小型主机；缓存不是持久数据库。

公开访问使用 [Nginx 示例](../packaging/nginx/hoshiguard.conf.example)。
HTTPS/WSS 证书由 Nginx 管理。浏览器跨域访问需在 ExecStart 增加
`--api-origin https://dashboard.example.org`，也适用于代理终止 TLS 后的 HTTPS Origin。
不接受通配符。CORS 和 Origin 检查不是认证；接口只有公开只读数据。
生产部署应只让 Nginx 访问 loopback API，限制连接/请求并配置 TLS。

升级：停止服务，备份配置和 HGRAW，更新代码，在 venv 中重新安装 `python[api]`，
再启动。0.3.0 不读取旧 NDJSON 恢复状态；内存缓存和进程序号重置。若需展示历史，
单独使用 `--replay evidence.hgraw --api`，状态始终标明 historical，不能与 live
串口同时启用。旧诊断参数迁到 `hoshiguard diagnose`；旧 systemd/env 应一并更新。

故障判断：disconnected 表示设备断开；waiting_hello 等待合法握手（固件周期 60s）；
stalled 表示连接尚在但 15s 无 Bridge 帧；no_l1s 表示 Bridge 有通信但 30s 无有效
L1S。no_l1s 不足以断定没有卫星，应检查天线、遮挡和 GNSS 配置。
无新灾害信息不等于故障；SLAS/Null 仍可证明 L1S 接收活跃。
