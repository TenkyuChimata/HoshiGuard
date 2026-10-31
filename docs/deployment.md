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

## SSD1306 OLED

SSD1306 128×64 の I2C モジュールを任意で接続できます。7-bit アドレスは `0x3C`、
SCL は PB6、SDA は PB7、I2C1 は Fast mode / duty 2 / 400 kHz です。
GND を共通にし、SCL/SDA は外部抵抗で **3.3 V** にプルアップしてください。
5 V にプルアップされたモジュールはそのまま接続しないでください。
電源とリセット回路はモジュールの仕様に従います。専用の OLED リセット端子は使用しません。
この機能は I2C1 と PB6/PB7 を専有します。USART2 PA2/PA3、PPS PA8、USART1 PA9/PA10 は従来どおりです。

```sh
# OLED 有効（既定の Bridge Binary 環境）
pio run -e genericSTM32F103C8-bridge-binary
# OLED を完全に除外する場合
pio run -e genericSTM32F103C8-bridge-binary-no-oled
```

`include/config.h` の `HOSHIGUARD_OLED` は、未指定なら `BRIDGE_BINARY_MODE` に従います。
独自環境では `build_flags` に `-D HOSHIGUARD_OLED=0` または `1` を指定できます。
Bridge Text、capture などの環境では既定で無効です。無効時は OLED 用オブジェクトも I2C 初期化も生成せず、
Bridge 1.0 のフレーム形式・バージョン・送信先は変えません。

画面は固定の 8 行・各 21 文字です。独自の 5×7 英数字字形に余白を加えた 6×8 セルを使い、
見出しを反転表示します。名称と `HOSHIGUARD_VERSION` の実際の値は常に最上段に表示し、
起動用の待機画面やページ切り替えによる受信の遅延は設けません。次は表示形式の合成例です。

```text
HOSHIGUARD 0.2.7
M10S:READY UBX:OK
PPS:OK TIME:CONF
L1S:LIVE N:42
AGE:1S MT44 DCX
BR TX:120 D:0
BAD U:0 L:0
RX ACTIVE
```

| 表示 | 意味 |
|---|---|
| `M10S` | 既存初期化状態の `INIT` / `READY` / `DEG` / `FAIL`。`DEG` は制限付き動作 |
| `UBX:OK` / `WAIT` / `LOST` | 直近 3 秒未満に checksum-valid UBX あり / 起動後まだなし / 過去にはあったが途絶 |
| `PPS:SEEN` / `OK` / `BAD` | PPS 観測直後 / 既存判定で正常間隔が 3 回連続 / 最近の間隔異常あり。1.8 秒以上エッジなしは `LOST`、未観測は `WAIT` |
| `TIME` | 既存の時刻品質を `NONE` / `UTC` / `PPS` / `TENT` / `LIMIT` / `CONF` / `WARN` で表示。UTC 有効・PPS のみ・暫定関連付け・制限付きロック・確認済みを区別 |
| `TIME:STALE` | 必要な UBX、UTC-valid NAV-PVT、TIM-TP、PPS の新しい証拠が不足。古い `CONF` を保持しない |
| `L1S:LIVE` / `WAIT` | PAB と CRC の両方が正しい L1S が 30 秒未満にあり / 待機。SLAS、Null、未対応 MT も受信証拠に含む |
| `N` / `AGE` | 起動後の有効 L1S 累積数（衛星コピーの重複も含む）/ 最後の有効 L1S からの経過秒。長時間なら時間・日単位。未受信は `--` |
| `MT43 DCR` / `MT44 DCX` | 最後の有効 L1S の MT。その他は MT 番号のみ。災害内容・イベントの存在を断定しない |
| `BR TX` / `D` | MCU が UART へ送り終えた Bridge フレーム数 / 起動後の累積破棄数。ホストの接続・受領確認ではない |
| `BAD U` / `L` | UBX checksum エラー数 / L1S PAB・CRC 検査失敗数の合計。同一フレームの両方の失敗は 2 件として数える |
| 最下段 | `CONFIGURING M10S`、通常の `WAIT L1S` / `RX ACTIVE`、または `!` 付きの反転した注意表示 |

`CONF` / `LIMIT` には最近の TIM-TP と正常 PPS も必要です。UBX、NAV-PVT、TIM-TP の鮮度上限は 3 秒です。
衛星数や測位状態は表示せず、Python 側の DCR/DCX デコーダーも再実装していません。
SFRBX 検証の待機期限が過ぎたという理由だけでは、衛星の受信故障と表示しません。
UBX が継続する場合、L1S の未受信・長い間隔は通常の `WAIT L1S` として扱います。
`! CONFIG E番号` は既存 `GnssErrorCode` の値、`! BR DROP HISTORY` / `! BR L1S DROP` は
累積破棄の履歴です。破棄が今も続いているという意味ではありません。`TX` は 99999、
破棄・検査失敗は 9999 を超えると `+` を付けて省略します。有効 L1S 数は 32-bit 上限で飽和します。
経過時間も飽和加算し、`millis()` の周回で古い受信が新しく見えないようにします。

状態の整形は 500 ms ごとです。前回と同じ行は送らず、変更行を 16 pixel-column ずつに分割します。
1 行は位置コマンド 1 転送とデータ 8 転送です。1024 byte の framebuffer は保持せず、
送信済み・次回表示用の文字列と転送中の 1 行を固定領域に保持します。動的メモリー確保はありません。
初期化時は表示を OFF にして全 8 行を書き、完了後に ON にします。

I2C は STM32F1 レジスターの状態機械で駆動します。各 loop の PPS 処理、USART2 の読み出し、
Bridge の pump の後に最大 1 段階・1 byte だけ進め、I2C 完了待ちループ、`delay()`、
割り込み禁止、ISR 内の I2C 操作は行いません。UART に未処理入力があれば新規転送を延期します。
1 転送全体の期限は 3 ms であり、3 ms のブロッキング待機ではありません。
期限の検査は次の loop 呼び出し時に行います。NACK、バスエラー、BUSY/STOP 固着、クロックストレッチの
期限超過では OLED のみを停止し、I2C1 をリセットして端子を解放し、30 秒後に全体を初期化し直します。
無接続時も同じ動作で、受信・PPS・Bridge を停止せず、USART1 に OLED 診断文字列を混在させません。
バス故障では画面の消去も保証できず、最後の表示が残る場合があります。

STM32 platform 15.6.0、STM32duino 2.4.0、GCC 9.2.1、既存の `-Os -flto` で、
同一の `SOURCE_DATE_EPOCH` を指定して clean build した比較値です。

| ビルド | Flash イメージ（byte） | 静的 RAM `.data + .bss`（byte） |
|---|---:|---:|
| 変更前の Bridge Binary | 43,476 | 10,488 |
| OLED 無効 | 43,476 | 10,488 |
| OLED 有効 | 48,068 | 11,036 |
| 増分 | +4,592 | +548 |

Flash は割り込みベクター・初期化配列を含む `.bin` 全体です。PlatformIO の表示値は、
有効時 47,780 / 無効時 43,188 byte で、この 288 byte を含みません。
静的 RAM 値は実行時のスタック最大使用量を示しません。リンカーは別途 heap/stack 用に約 1.5 KiB を予約します。
無効ビルドには OLED シンボルが残らず、この条件では変更前と `.bin` の SHA-256 も一致しました。
ビルド日時・ツールチェーンが異なる場合、サイズは変わり得ます。

ドライバー・字形は本プロジェクトで独立実装しています。
参考として AnyShake Explorer の [SSD1306 初期化](https://github.com/anyshake/explorer/blob/master/firmware/User/Src/ssd1306/utils.c)、
[描画](https://github.com/anyshake/explorer/blob/master/firmware/User/Src/ssd1306/display.c)、
[字形](https://github.com/anyshake/explorer/blob/master/firmware/User/Inc/ssd1306/font.h)、
[I2C](https://github.com/anyshake/explorer/blob/master/firmware/Core/Src/i2c.c) を確認しましたが、
AGPL のコード・字形・ロゴは取り込んでいません。HAL の逐 byte 書き込み、1 秒の通信期限、RTOS の画面処理は移植していません。
コマンドは [SSD1306 データシート](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf) に基づきます。

ホストテストでは初期化、転送状態、画面メモリー、変更検出、欠落・NACK・固着・再接続、
文字境界、カウンター・時刻周回、実 UBX fixture と Bridge キューの併用を検証します。
これらはソフトウェア検証です。実機での表示方向・コントラスト・電源投入、400 kHz の立ち上がり、
配線長・プルアップ、クロックストレッチ、切断・再接続、USART2 の overrun、PPS の遅延と Bridge の
連続負荷試験は別途必要です。ビルド成功を実機試験完了とは扱いません。

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
