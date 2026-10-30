# デプロイと受信診断

## systemd での運用

Python 3.10 以降、`python3-venv`、シリアルデバイスの読み書き権限、Bridge Binary ファームウェアが必要です。
以下は Debian/Linux の例です。リポジトリを `/opt/hoshiguard` に配置し、サービスユーザーから読めるようにしてください。
リポジトリのルートで実行し、パスとデバイス名は環境に合わせて変更します。

```sh
sudo apt install python3-venv
sudo useradd --system --home-dir /var/lib/hoshiguard --groups dialout hoshiguard
python3 -m venv /opt/hoshiguard/.venv
/opt/hoshiguard/.venv/bin/pip install '/opt/hoshiguard/python[api]'
sudo install -d /etc/hoshiguard
sudo install -m 640 -o root -g hoshiguard packaging/systemd/hoshiguard.env.example /etc/hoshiguard/hoshiguard.env
sudo install -m 644 packaging/systemd/hoshiguard.service /etc/systemd/system/
# HOSHIGUARD_PORT を実際のデバイスに合わせて編集
sudoedit /etc/hoshiguard/hoshiguard.env
sudo systemctl daemon-reload
sudo systemctl enable --now hoshiguard
curl http://127.0.0.1:8000/api/v1/status
journalctl -u hoshiguard -f
```

設定の基になるファイルは [service](../packaging/systemd/hoshiguard.service) と [env](../packaging/systemd/hoshiguard.env.example) です。
デバイス名は `/dev/serial/by-id/` の安定したパス、または udev で設定した `/dev/hoshiguard` を推奨します。
起動時にデバイスがなくても継続して再接続し、有効な HELLO を待ちます。HELLO の送信周期は 60 秒です。
同じシリアルポートを複数プロセスで同時に読み取らないでください。

既定では業務ログ・session/health/PID ファイルを作成しません。PID は systemd が管理し、
健全性は `/api/v1/status` で取得します。シリアル・解析エラーは journal に記録し、公開状態にはデバイスパスや例外本文を含めません。

## 証跡の保存とアップグレード

任意の HGRAW 保存を有効にする例:

```sh
hoshiguard --port /dev/hoshiguard --api --raw-archive captures/station.hgraw
```

systemd では `sudo systemctl edit hoshiguard` で `ExecStart=` を一度空にし、元のコマンドを再指定して
`--raw-archive /var/lib/hoshiguard/evidence.hgraw` を追加します。変更後は `daemon-reload` とサービス再起動が必要です。
履歴件数も同様に `--history-limit 256` などで指定できます（1–4096）。

HGRAW は既定で 256 MiB または 24 時間を境に、書き込み時に分割します。fsync の既定間隔は 30 秒ですが、
書き込み駆動であり、無通信時の定期フラッシュではありません。終了・分割時にも fsync し、SHA-256 ファイルを作成します。
空き容量が予約分の 64 MiB を下回るとエラー終了します。古い分割ファイルは自動削除しないため、管理者が保持期限を設定してください。
突然の電源断では直前のバッファを失う場合があります。読み取り側は破損・途中切断したレコードを検出して次のレコードへ復帰します。
HGRAW の構造は[プロトコル資料](protocol.md#hgraw-証跡形式)を参照してください。

アップグレード時はサービスを停止し、設定と HGRAW を退避してからコードを更新し、venv 内で `python[api]` を再インストールします。
0.3.0 は旧 NDJSON を復元状態として読み込みません。メモリ履歴と `sequence` は再起動でリセットされます。
過去の証跡は `hoshiguard --replay evidence.hgraw --api` で別途再生できます。
EOF 後も API は参照できますが、常に `historical` として扱い、live シリアルとは同時指定できません。
旧診断用 CLI の引数は `hoshiguard diagnose` に移っているため、旧 service/env も確認してください。

## ネットワーク公開

アプリケーションの既定は `127.0.0.1:8000` です。[Nginx 設定例](../packaging/nginx/hoshiguard.conf.example)は
HTTP と WS Upgrade、要求数・接続数制限を示すものです。例の `listen 80` 自体は TLS を有効にしません。
公開時はプロキシで証明書と HTTPS/WSS を設定し、アプリケーションのポートへ直接外部接続させないでください。

別オリジンのブラウザーを許可する場合は、起動引数に `--api-origin https://dashboard.example.org` を追加します。
TLS をプロキシで終端する場合も、ブラウザーが使う HTTPS Origin を指定します。複数指定でき、ワイルドカードは使えません。
Origin/CORS 検査は認証ではありません。API は公開用の読み取り専用情報のみを提供します。
詳細な制限は [API](api.md#健全性と資源上限) を参照してください。

## 障害の切り分け

| 状態 | 意味・確認項目 |
|---|---|
| `disconnected` | デバイス切断。電源、ケーブル、デバイスパスと権限を確認 |
| `waiting_hello` | 有効な HELLO 待ち。ファームウェア、baud、送信周期を確認 |
| `stalled` | 接続中だが 15 秒間 Bridge フレームなし |
| `no_l1s` | Bridge は通信中だが 30 秒間有効 L1S なし。アンテナ、遮蔽、GNSS 設定を確認 |

災害通報がないだけでは故障になりません。SLAS/Null も有効 L1S として受信状態を更新します。
`no_l1s` だけで衛星が全く受信できないとは断定できません。

## 任意の診断

詳細観測、Human 出力、証跡監査は `diagnose` で使います。公開 API と同じ UnifiedAlert を使い、別のデコーダーはありません。

```sh
hoshiguard diagnose --help
hoshiguard diagnose --bridge-file session.hgraw --human
hoshiguard diagnose --bridge-file session.hgraw --semantic-observations --ndjson alerts.ndjson
hoshiguard diagnose --bridge-file session.hgraw --all-observations --ndjson observations.ndjson --json-report counts.json
hoshiguard diagnose --capture-log legacy-capture.log --human
```

| モード | 内容 |
|---|---|
| `all-observations` | Python 検証を通過した各フレームの観測記録 |
| `semantic-observations` | 同一 semantic fingerprint v2 は一度のみ出力。診断時の既定 |
| `events-only` | 明示的な業務 action。SLAS/Null/予約 MT を発表扱いしない |
| `repeat-summary` | 再生末尾の有界な反復集計。追加の災害イベントではない |

詳細 NDJSON には UBX/L1S raw、内部パス、session 情報が含まれ得ます。`--omit-raw-json` で raw、
`--no-session-records` で session_start/end を省略できます。診断形式と HGRAW 1.x の移行は不要です。
`--output-directory`、`--session-id`、`--rotate-size`、`--rotate-interval`、`--fsync-interval`、
`--health-file`、`--pid-file`、診断用ディスク方針はこの入口で指定します。
本番シリアルを同時に開かず、保存済み HGRAW を分析してください。

MCU の text/debug/capture/capture-l1s/loopback/json 環境は [platformio.ini](../platformio.ini) から明示的に選択します。
Binary Bridge では USART1 をバイナリ専用とし、テキスト出力を混在させません。
デバッグ取得には位置情報が含まれる可能性があるため、公開前に次の処理と目視確認を行ってください。

```sh
python3 tools/sanitize_capture.py capture.log --strip-nav-pvt --output capture-public.log
```

関連ツールと検証手順は[テスト資料](../test/fixtures/README.md)にまとめています。
