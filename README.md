# HoshiGuard v0.2.7

HoshiGuard は、STM32F103C8T6 と u-blox MAX-M10S を対象とした、QZSS L1S の受信・診断・ホスト側デコードを行うプロジェクトです。v0.2.7 の正式な処理経路は次のとおりです。

```text
MAX-M10S → STM32F103C8T6 → COBS/CRC32 Bridge → Python
          UBX/L1S trust boundary             DCR/DCX + events + Human/NDJSON/archive
```

STM32 は、GNSS 設定、UBX、厳密な SFRBX/L1S フィルタリング、PAB/MT/CRC、PPS/TIM-TP の証拠取得、および完全な RXM-SFRBX の転送を担当します。一方で、大規模な地域/Hazard テーブルの保持、完全な業務イベント状態機械、正式な NDJSON の生成は行いません。Python は完全な UBX から UBX checksum、`numWords`、バイト順、250 bit、PAB/MT/CRC を独立して再検証し、STM32 側と Python 側の判定が一致した場合にのみ MT43/MT44 の意味層へ進みます。Human 出力と NDJSON は、同一の不変 `UnifiedAlert` から生成されます。

本プロジェクトでは Arduino `String`、動的 JSON、ループ内の `new/delete`、ブロッキング型の GNSS 待機を使用しません。UBX、設定、raw ログ、PPS はすべて固定バッファとノンブロッキング状態機械で処理します。

> 安全に関する注意：QZSS サービス、GNSS 受信経路、および本装置はいずれも、見逃し、遅延、誤出力を起こす可能性があります。本装置を生命安全に関わる唯一の警報源として使用してはなりません。実運用では、独立した警報経路とフェイルセーフ対策を必ず用意してください。

## Reliable Bridge と Python

本番用の `genericSTM32F103C8-bridge-binary` は USART1 へ COBS record のみを出力し、`00` でフレームを区切ります。論理 frame は little-endian の固定ヘッダと CRC-32/ISO-HDLC を使用します。プロトコルバージョンは 1.0 で、HELLO、L1S_FRAME、STATUS、TIME_STATUS、ERROR、HEARTBEAT に対応します。詳細なレイアウトと flags は [`docs/bridge_protocol.md`](docs/bridge_protocol.md) を参照してください。L1S_FRAME には 32-byte の L1S だけではなく、実際の完全な 52-byte RXM-SFRBX が含まれるため、第 9 word と UBX checksum は失われません。

`genericSTM32F103C8-bridge-text` は picocom による診断用です。各 L1S は、1 件の不可分なテキスト record として出力されます。

```text
BRIDGE_L1S seq=12 timestamp_us=123456 pps_seq=8 svId=3 gnssId=5 sigId=1 numWords=9 pab=53 mt=43 ubx_checksum=ok l1s_crc=ok flags=... len=52 data=B562...
```

Python パッケージをインストールします。リアルタイムのシリアル受信には、追加で pyserial が必要です。

```sh
python3 -m pip install -e ./python
python3 -m hoshiguard --port /dev/ttyUSB3 --baud 115200 \
  --human --ndjson alerts.ndjson --raw-archive captures/session.hgraw

python3 -m hoshiguard --bridge-file captures/session.hgraw \
  --semantic-observations --human --ndjson replay.ndjson
python3 -m hoshiguard --capture-log capture-v024-l1s-20260714-192545.log --human
```

Human 出力と NDJSON は同じデータ源から生成されます。以下は出力形式の例です。`TEST` は Actual のイベント空間には入りません。SLAS/NULL は `alert=false` および `Action: none` として表示されます。

```text
[TEST][DCX][SEVERITY=3]
Provider: raw=2
Hazard: Missile attack (raw=8)
Action: issued
Validity: complete; UBX=True; L1S_CRC=True
Repeated: 1 frame(s)
```

```json
{"schema":"hoshiguard.alert.v1","source":"QZSS_L1S","service":"DCX","spec":"IS-QZSS-DCX-004/CAMF-1.2","receipt":{"bridge_sequence":123,"timestamp_us":123456789,"pps_sequence":88,"sv_id":7,"pab":83,"mt":44},"validity":{"bridge_crc":true,"ubx":true,"l1s_crc":true,"structure":true,"semantics":true,"complete":true},"status":"test","action":"issue","identity":{"event_key":"DCX:test:8:...","confidence":"limited"},"category":{"code":8,"token":"MISSILE_ATTACK","known":true},"event":{"disposition":"issued","repeat_count":1}}
```

実際の NDJSON には、さらに times/areas/payload/unknown_fields と、設定可能な raw evidence が含まれます。各行は `json.dumps(..., allow_nan=False)` によって一度に生成され、途中で切れたオブジェクトを書き込むことはありません。

### Observation、semantic、event の 3 種類のストリーム

v0.2.7 では、伝送観測と業務イベントのライフサイクルを明確に分離しています。互換性維持のため、既定値は `--semantic-observations` です。

```sh
PYTHONPATH=python python3 -m hoshiguard --bridge-file session.hgraw \
  --all-observations --ndjson all.ndjson
PYTHONPATH=python python3 -m hoshiguard --bridge-file session.hgraw \
  --semantic-observations --repeat-summary --ndjson semantic.ndjson
PYTHONPATH=python python3 -m hoshiguard --bridge-file session.hgraw \
  --events-only --repeat-summary --ndjson events.ndjson
```

- `--all-observations` は、Python 側で再検証されたすべての frame observation を出力します。
- `--semantic-observations` は、v2 canonical semantic fingerprint ごとに 1 回だけ出力します。
- `--events-only` は、仕様上の根拠があるライフサイクルのみを出力します。SLAS と DCX Null はイベントストリームに入りません。
- `--repeat-summary` は、有界な repeat count、first/last、satellites、PAB の要約を出力します。

raw archive は、重複排除によって frame を削除することはありません。`frame_fingerprint` は完全な UBX と伝送上の差異を保持します。`semantic_fingerprint_version=2` は PAB、svId、sequence、timestamp、PPS、raw/checksum を除外します。`event_key` には説明可能な業務フィールドのみを使用します。仕様にグローバルなイベント ID が存在しない場合は、誤結合を避けるため `identity_confidence=limited` のままとします。DCX Null は常に `alert=false,action=none` であり、issued を生成しません。

DCX EX9 の 64-bit target-area list を含む任意幅フィールドは、Python の
MSB-first bit reader で 250-bit L1S 境界内に限って読み取ります。HGRAW は
意味デコーダより先に保存されます。個々の DCR/DCX frame が明示的な
`L1sError` / semantic decode error になった場合は、その frame の Human/NDJSON
だけを省略し、`semantic_decode_errors` と直近エラーを health に記録して後続
frame を処理します。未知のプログラム不具合を広い例外捕捉で隠しません。

v0.2.7 の各業務オブジェクトには、`stream_mode`、`producer_version`、`semantic_fingerprint_version`、`bridge_protocol_version`、`session_id` が含まれます。ファイルは既定で、有効な `hoshiguard.session.v1` の `session_start` から始まり、正常終了時には `session_end` で閉じられます。旧コンシューマは未知の `record_type` を無視するか、明示的に `--no-session-records` を使用できます。ファイル名を出力モードの証拠として扱うことはありません。

### 1 時間の実機 Binary Bridge 検証

```sh
python3 tools/validate_bridge_session.py v025-session.hgraw \
  --ndjson alerts-v025.ndjson \
  --json-report reports/v025-session-report.json \
  --markdown-report reports/v025-session-report.md
python3 tools/import_bridge_session_fixture.py v025-session.hgraw --check
```

このツールは、archive CRC、COBS、Bridge CRC、sequence/HELLO epoch、完全な UBX、SFRBX、250-bit、PAB/MT/L1S CRC、STM32 flags をストリーミングで検証します。実機レポートでは、11,466 records、7,355 L1S、sequence 351..11816 を確認し、gap/duplicate/out-of-order はありませんでした。先頭と末尾の STATUS による閉区間では、7,333 L1S と generated/sent delta が完全に一致し、Bridge/L1S/Critical drop はすべて 0 でした。詳細は [`docs/real_bridge_session_validation.md`](docs/real_bridge_session_validation.md) を参照してください。Bridge wire protocol は引き続き 1.0 です。

### 24 時間の実機 Bridge soak test

`captures/soak-24h.hgraw` は、firmware 0.2.5、Bridge 1.0 による実機証拠です。v0.2.6/v0.2.7 の firmware 自体を 24 時間長時間試験したものではありません。87,249.121 秒の間に、243,952 archive records と 146,520 L1S_FRAME が archive/COBS/Bridge/UBX/L1S の各検証を連続して通過しました。sequence 2708..246659 に gap、duplicate、out-of-order はなく、drop counter はすべて 0 でした。8,727 個の隣接 STATUS ウィンドウでは、generated/observed と、送信待ちキュー中の record を考慮した sent の閉合がすべて成立しました。240 ウィンドウで raw sent snapshot が 1 件だけ異なるのは、STATUS エンコード時点で 1 record がキュー内を移動中だったためであり、frame loss ではありません。

```sh
python3 tools/analyze_soak_session.py captures/soak-24h.hgraw \
  --json-report reports/soak-24h-report.json
python3 tools/audit_ndjson_stream.py captures/soak-24h-all.ndjson \
  --archive captures/soak-24h.hgraw
python3 tools/import_24h_session_fixture.py captures/soak-24h.hgraw --check
```

旧 `soak-24h-all.ndjson` の実体はすべて `semantic_observation` であり、mode metadata はありません。37,911 行が L1S 数より少ない理由は、当時の既定 semantic 重複排除と、2 つのファイルの終了境界が異なるためです。Bridge loss ではありません。詳細は `docs/evidence_provenance.md` および各 `soak-24h-*` レポートを参照してください。

### 長期運用、ローテーション、ヘルスチェック

```sh
PYTHONPATH=python python3 -m hoshiguard \
  --port /dev/hoshiguard --baud 115200 \
  --semantic-observations --repeat-summary \
  --output-directory /var/lib/hoshiguard --session-prefix station-a \
  --rotate-interval 24h --rotate-size 256M --fsync-interval 30s \
  --health-file /run/hoshiguard/health.json \
  --pid-file /run/hoshiguard/hoshiguard.pid
```

ローテーションは完全な record 境界でのみ実行されます。SHA-256 は書き込み時に増分計算され、閉じたセグメントは flush/fsync の後に `.sha256` sidecar を直接生成します。ローテーション時に大きなファイル全体を再走査することはありません。semantic/event 状態はローテーションによってリセットされません。既定のディスクポリシーは、空き容量 1 GiB で警告、512 MiB で派生 NDJSON を停止して HGRAW を優先保存、128 MiB で安全停止します。archive record の途中だけを黙って書き込むことはありません。

既定では raw evidence を優先します。特殊な構成では `--disk-critical-action drop-archive` を明示して NDJSON を優先保存するか、`stop` を指定して critical 閾値で両方の出力を即時停止できます。systemd の導入方法と安定した `/dev/hoshiguard` udev symlink の設定は [`docs/systemd_deployment.md`](docs/systemd_deployment.md) を参照してください。

### 2026-07 DCR 公式試験シナリオ

`details_202607.pdf` は official scenario oracle としてのみ使用し、DCR-016 の代替にはしません。また、バイナリデータの捏造にも使用していません。manifest は、2026-07-14 と 2026-07-30 の 2 つの JST/UTC 時間帯、Rc=7、7 シナリオ、9 カテゴリ、南海トラフ Pn/Pm 27 ページ、降灰 5 通/18 地点、および津波/洪水の issue/update/clear をカバーします。

```sh
python3 tools/validate_dcr_test_scenario.py captures/dcr-202607.hgraw \
  --manifest test/fixtures/dcr_202607/scenario_manifest.json
```

出力では `not_received`、decode failure、field mismatch、valid を区別します。実際の高優先度メッセージが試験時間帯を上書きする可能性があるため、not received は decoder failure を意味しません。次回の収集コマンドは [`docs/dcr_202607_test_plan.md`](docs/dcr_202607_test_plan.md) を参照してください。

シリアルポートが切断されても receiver はクラッシュしません。途中まで受信した frame を破棄し、自動再接続し、次の HELLO を待って、新しい sequence epoch を HELLO から確立します。通常の sequence gap、duplicate、out-of-order は別々に集計されます。再起動後の sequence リセットを frame loss と誤認しません。raw archive は完全な Bridge wire frame、PC receive time、STM32 timestamp を保存し、各 archive record にも CRC を付加します。ファイルの一部が破損した場合は、次の `HGRF` を走査して復旧します。

MT のルーティングは、43=DCR、44=DCX、47–51=SLAS かつ `alert=false`、63=NULL、40/41=reserved です。その他は raw のまま保持します。frame 数は災害イベント数ではありません。異なる衛星からの重複、放送の再送、意味的重複、update、cancel、clear は階層を分けて扱い、Test/Exercise と Actual は別のイベント空間を使用します。`unknown`/`reserved` を 0 で埋めたり、破棄したりしません。

仕様一覧、情報源監査、フィールド追跡、正直な対応範囲は、次の文書に記載しています。

- [`docs/spec/SPEC_INVENTORY.md`](docs/spec/SPEC_INVENTORY.md)
- [`docs/source_audit.md`](docs/source_audit.md)
- [`docs/dcr_mt43_traceability.md`](docs/dcr_mt43_traceability.md)
- [`docs/dcx_mt44_traceability.md`](docs/dcx_mt44_traceability.md)
- [`docs/support_matrix.md`](docs/support_matrix.md)

DCR-016 の 12 個の有効な災害カテゴリには、それぞれ独立した payload 型とカテゴリ別 decoder があります。南海トラフのテキストは、固定容量とタイムアウト付きで再構成します。DCX は CAMF A1–A18、A4 1–113、EX1–EX9 に対応し、EX10 reserved も保持します。DCX-004 は単一 MT44 の CAMF+Extended Message であり、仕様上の跨ぎ frame 再構成は必要ありません。EX11 は地域固有の 68-bit 拡張で、現時点では raw のみです。また、本リポジトリでは最新の JIS X 0402 公式データセットを独立監査していないため、市区町村名称テーブルは partial としています。この 2 つの未解決項目があるため、本プロジェクトは「すべての国、すべての library の DCX を無条件に完全対応」とは主張しません。

## v0.2.4 実 QZSS 回帰ベースライン

`capture-outdoor.log` は、実機 STM32F103C8T6 + MAX-M10S SPG 5.10 による屋外収集データです。完全で checksum が正しい `UBX_RAW` から独立再計算した結果、5,366 個の UBX packet はすべて検証に成功しました。そのうち 3,072 個は完全な QZSS RXM-SFRBX で、2,657 個が `sigId=1,numWords=9` の L1S、415 個が `sigId=0,numWords=10` の L1C/A です。2,657 個の L1S は、PAB と CRC がすべて有効でした。MT 分布は 43:672、44:616、47:51、48:89、49:85、50:1,144 です。これらの数値はメッセージ種別の件数であり、同数の実災害が発生したことを意味しません。

元ファイルの先頭 78,626 bytes は、picocom/HoshiGuard が記録を開始する前のバイナリノイズです。ツールは bytes モードで最初の信頼できる record を特定し、ノイズを無理に Unicode としてデコードしません。最後の STATUS では firmware が 3,440 個の L1S を処理したと報告していますが、ファイルに完全な L1S UBX として保存されているのは 2,657 個のみで、少なくとも 783 個、22.76% が欠落しています。ログ全体の累積 drop は 8,948 です。この差分は、低損失モードを追加する直接の根拠であり、衛星受信失敗を示すものではありません。

実データ fixture は `test/fixtures/real/` にあります。10 個の MT/PAB 正常ベクトル、1 個の実 L1C/A 除外ベクトル、さらに synthetic-derived/corrupted と明示した 3 個の拒否ベクトルがあります。これらは完全な UBX から `UbxParser → UbxFrameDispatcher → SFRBX → L1S → PAB/MT/CRC/重複排除` を通して回帰し、UART バイトストリーム経路を省略しません。fixture には NAV-PVT、緯度経度、正確な地点情報を保存しません。

## 配線

| MAX-M10S / デバッガ | STM32F103C8T6 | 説明 |
|---|---|---|
| MAX-M10S TX | PA3 / USART2_RX | GNSS → MCU |
| MAX-M10S RX | PA2 / USART2_TX | MCU → GNSS |
| MAX-M10S TIMEPULSE | PA8 | 1PPS、立ち上がりエッジ EXTI |
| MAX-M10S GND | GND | 必ず共通 GND にする |
| MAX-M10S VCC | 適切な 3.3 V 電源 | 使用するモジュール基板の仕様に従う |
| USB-UART RX | PA9 / USART1_TX | デバッグ出力、115200 8N1 |
| USB-UART TX | PA10 / USART1_RX | デバッグ UART RX、現在は未使用 |

すべての信号は 3.3 V ロジックでなければなりません。5 V UART を MAX-M10S または STM32 に直接接続しないでください。アンテナバイアスとモジュール電源については、使用するキャリア基板と MAX-M10S Integration Manual に従ってください。

本プロジェクトで使用する STM32duino Core 2.4.0 の generic F103C8 variant では、`Serial2` が PA3 RX/PA2 TX として定義されています。デバッグポートは `HardwareSerial DebugSerial(PA10, PA9)` を使用します。このコンストラクタの引数順は `(RX, TX)` です。

## ビルド、書き込み、モニタ

```sh
pio run
pio run -e genericSTM32F103C8-capture
pio run -e genericSTM32F103C8-capture-l1s
pio run -e genericSTM32F103C8-json
pio run -e genericSTM32F103C8-debug
pio run -e genericSTM32F103C8-loopback
pio run -e genericSTM32F103C8-debug-38400
pio run -e genericSTM32F103C8-bridge-binary
pio run -e genericSTM32F103C8-bridge-text
pio test -e native

pio run -e genericSTM32F103C8 -t upload
pio device monitor -b 115200
```

各環境の説明：

- `genericSTM32F103C8`：既定の人間可読出力です。
- `genericSTM32F103C8-capture`：UBX/SFRBX/L1S raw record を有効にし、通常の frame ごとのログを抑制します。
- `genericSTM32F103C8-capture-l1s`：長時間・低損失モードです。各 QZSS L1S につき、完全な UBX を含むコンパクト record を 1 件だけ保存し、L1S 用に 3 個のキュースロットを予約します。既定では NAV-PVT raw、QZSS 以外の SFRBX、毎秒 PPS、重複する SFRBX/L1S テキストを保存しません。
- `genericSTM32F103C8-json`：旧 MCU 側の軽量 JSON 互換テストを維持します。正式な完全 NDJSON は Python が生成します。
- `genericSTM32F103C8-debug`：初回の現地調整用です。詳細な状態、完全な MON-VER extension、CFG 判定、ACK/NAK、すべての SFRBX header、選択的 raw capture を出力します。
- `genericSTM32F103C8-debug-38400`：現場用セーフモードです。完全な debug/capture を有効にしつつ、`GNSS_CHANGE_BAUD=0` として、実測で通信できた 38400 を維持します。
- `genericSTM32F103C8-loopback`：MAX-M10S を切り離し、PA2/PA3 を一時的に短絡して、MCU USART2 とピン割り当てを検証します。1 回だけ実行し、繰り返しログを出しません。
- `genericSTM32F103C8-bridge-binary`：正式な高信頼 Bridge です。テキストを混在させず、L1S 予約スロットを 4 個持ち、privacy mode を既定で有効にします。
- `genericSTM32F103C8-bridge-text`：人手での調整用 Bridge です。下位サービスと検証証拠のみを表示し、完全な災害意味解析は行いません。
- `native`：ホスト側のプロトコル、設定、ログ、注入、PPS テストです。

ST-Link の CPUTAPID override は STM32 共通セクションにのみ設定し、`native` 環境へ誤って適用されないようにしています。

### v0.2.7 の最終リソース使用量と負荷予算

以下は、実際にリンクされた ELF に対する PlatformIO の Flash/RAM 値です。`.data/.bss` には、リンカスクリプトが別途確保する 1.5 KiB の user heap/stack reservation は含まれません。Bridge 自体は heap を使用しませんが、Arduino Core はこの領域を保持します。

| 環境 | Flash | RAM | `.data` | `.bss` |
|---|---:|---:|---:|---:|
| default | 47,476 | 7,612 | 632 | 6,980 |
| capture | 47,024 | 10,524 | 632 | 9,892 |
| capture-l1s | 45,692 | 11,128 | 632 | 10,496 |
| json compatibility | 47,404 | 7,604 | 632 | 6,972 |
| debug | 51,256 | 11,172 | 632 | 10,540 |
| loopback | 23,836 | 7,284 | 628 | 6,656 |
| debug-38400 | 51,196 | 11,172 | 632 | 10,540 |
| bridge-binary | 43,200 | 10,488 | 636 | 9,852 |
| bridge-text | 45,684 | 11,960 | 632 | 11,328 |

Binary Bridge は v0.2.4 default より Flash が 4,248 bytes 少なくなっています。一方、完全 frame の原子性、7 スロットキュー、4 個の L1S 予約スロットのために RAM を 2,876 bytes 追加しています。最大のプロジェクト静的オブジェクトは 3,936-byte の `BridgeTransport` です。これにはキュー、論理 frame/COBS/L1S の固定作業領域、統計が含まれ、完全 frame 用配列を実行時スタックに積まない設計です。v0.2.4 の同一環境と比較すると、その他の Flash 変化は -48～+28 bytes で、RAM はほぼ変わりません。

`tools/benchmark_bridge.py` は v0.2.4 の実到着間隔を使用して、決定論的に 1 時間分を再生します。8,332 個の L1S が generated/sent となり、L1S drop=0、critical drop=0、sequence gap=0、semantic alert drop=0、queue high watermark=4 でした。平均は 293.873 B/s、ピークは 776 B/s です。115200 8N1 の理論上の有効バイトレートは 11,520 B/s であり、平均/ピーク使用率はそれぞれ約 2.6%/6.7% です。長時間スループット不足を隠すために 460800 を使用する必要はありません。最終ホスト実行時の Python オフライン処理は約 6,152 frame/s でした。decoder の p50/p95/p99 は 162.81/170.54/175.08 us、意味層は 797.11/932.34/950.08 us、NDJSON は 352.52/441.16/449.29 us です。完全な機械可読レポートは [`docs/performance_v025.json`](docs/performance_v025.json) にあります。これらはホスト側シミュレーションであり、新しい Bridge firmware の 1 時間実機運転証明ではありません。

## 電源投入時の状態と設定検証

正常起動時は、次の順序で状態が表示されます。

```text
DetectingBaud
PollingMonVer
ReadingConfig
ApplyingConfig       # 値が不一致、または VALGET fallback の場合のみ表示
WaitingForRestart    # signal 設定変更後、約 600 ms
VerifyingBaud        # 実際に baud rate を変更した場合のみ表示
VerifyingSfrbx
Ready
```

各状態遷移は 1 行で出力されます。

```text
GNSS_STATE from=ReadingConfig to=ApplyingConfig reason=value differs time_ms=... retry=...
```

失敗時は `Degraded` または `Failed` に移行することがあります。`Degraded` でも受信は継続します。例として、任意の TIMEPULSE key が未対応の場合や、設定完了後 15 秒間 SFRBX が到着しない場合があります。`Failed` は、すべての候補 baud rate で MON-VER を取得できないなど、初期化を継続できないエラーに使用します。

firmware は 9600、38400、115200 の順に `UBX-MON-VER` を poll し、有効な応答を受信した場合にのみ双方向通信の識別成功と判断します。checksum が正しい任意の UBX は baud rate の証拠として使用できます。一方、checksum が正しい NMEA sentence が 2 件以上あっても、得られる判定は `NMEA-only inferred baud` であり、UBX detected として扱いません。`MON-VER sw=... hw=...` により、モジュールと firmware の情報を確認できます。

### USART2 双方向診断

`genericSTM32F103C8-debug` では、既定で `GNSS_RX_PROBE=1` と `GNSS_TX_ECHO_HEX=1` を有効にします。各候補 baud rate について、ノンブロッキングの 450 ms ウィンドウ内で全 RX byte を集計し、先頭 128 bytes を保存します。すべての byte は同時に既存の UBX parser にも渡されます。候補切り替え時には、未完了の UBX/NMEA/probe の局所状態のみをリセットし、累積統計はリセットしません。ASCII では `CR/LF/TAB` をエスケープし、その他の制御 byte は `.` に置換します。HEX は常に大文字で、32 bytes ごとに改行します。

```text
GNSS_TX time_ms=... baud=9600 type=MON_VER_POLL len=8 write_result=8 data=B5620A0400000E34
GNSS_RX_PROBE baud=9600 bytes=... printable=... dollar=... crlf=... nmea_lines=... nmea_valid=... nmea_invalid=... nmea_incomplete=... ubx_sync=... ubx_valid=... zeros=... ff=... classification=LikelyNmea evidence=valid_nmea_checksum
GNSS_RX_ASCII baud=9600 offset=0 data="$GNGGA,...\\r\\n..."
GNSS_RX_HEX baud=9600 offset=0 data=24474E...
GNSS_BAUD_RESULT baud=9600 rx=... ubx=... nmea=... mon_ver_sent=1 write_result=8 mon_ver_response=0 timeout=0
GNSS_BAUD_TIMEOUT baud=9600 mon_ver_response=0
```

MON-VER poll の完全なソフトウェア生成値は `B5 62 0A 04 00 00 0E 34` で固定です。`serial_.write()` の戻り値は `tx_packets/tx_req/tx_written/tx_short` に計上され、short write は `GNSS_ERR_TX_SHORT_WRITE` を発生させます。firmware は能動的に `flush()` を呼びません。STM32duino 2.4.0 の `HardwareSerial::end()` は、TX 完了を待ってから RX をクリアします。候補切り替えは 900 ms の poll timeout 後に行われるため、直前の 8-byte poll はすでに送信キューを離れています。

以下の分類は保守的な証拠であり、電気測定そのものではありません。

- `ValidUbx`：checksum が正しい UBX が 1 件以上あります。
- `LikelyNmea`：`$`、CRLF、sentence ID があり、checksum が正しい状態です。NMEA framing が 2 回繰り返される場合も、より弱い証拠として使用できます。位置が未確定の GGA/RMC も認識できます。
- `LikelyText`：大部分が印字可能なテキストですが、十分な NMEA 検証証拠がありません。
- `LikelyWrongBaud`：サンプル長が十分で、プロトコル証拠がなく、上位 bit/非印字 byte または `00/FF` が大量にあります。単一の破損 byte では発生しません。
- `UnknownBinary`：継続的なバイナリですが、上記の証拠条件を満たしません。`NoData` はウィンドウ内に byte がなかったことを示します。

代表的な現場判断：

- `LikelyNmea` + MON-VER timeout：MAX-M10S TX → PA3 は正常で、その RX baud rate も正しい可能性が高い状態です。PA2 → MAX-M10S RX、モジュール UART の UBX input、実際のポート、電圧レベル、共通 GND を優先して確認してください。ロジックアナライザまたは 3.3 V USB-TTL を使用すると、PA2 で `B5620A0400000E34` が観測されるはずです。
- `LikelyWrongBaud`：他の候補結果と比較し、USB-TTL でモジュール出力を直接確認してください。
- `NoData`：モジュール電源、共通 GND、TX/RX の交差接続、PA3 の割り当て、モジュールが実際に UART を使用しているかを確認してください。
- `ValidUbx` だが MON-VER がない：class/id dispatch、MON-VER payload、送信経路を確認してください。屋内で衛星が見えないこととは無関係です。

USB-TTL は必ず 3.3 V TTL を使用し、システムと共通 GND にしてください。従来の ±12 V RS-232 は使用できません。loopback mode では、先に MAX-M10S を切り離してから PA2/PA3 を短絡してください。`00 55 AA FF` と MON-VER poll を送信し、500 ms 以内に `LOOPBACK PASS/FAIL`、expected、received を 1 回だけ出力します。loopback は STM32 USART2 とピンだけを検証し、MAX-M10S は検証しません。

### CFG-VALGET / CONFIG_ONLY_ON_CHANGE

`CONFIG_ONLY_ON_CHANGE=1` は既定で有効です。各 key について、最初に UBX-CFG-VALGET (`06 8B`) を送信して RAM layer を読み取ります。request version は 0、response version は 1 です。response の byte 4 以降には、little-endian の U4 key と、key によって幅が決まる value が格納されます。ログには key ID、required/optional、現在値、目標値、変更の有無、結果が表示されます。

```text
CFG key=0x10310014 name=CFG-SIGNAL-QZSS_L1S_ENA class=required current=1 target=1 change=no result=verified
CFG VALSET ACK key=0x... name=...
CFG VALSET NAK key=0x... name=... retry=...
```

値が一致している場合は VALSET を省略します。不一致の場合は VALSET を行い、ACK 後に再度 VALGET で検証します。VALGET timeout/NAK が発生した場合は、その key に対して VALSET へ fallback します。有限回の retry を使い切った場合、required key は初期化を degraded にします。optional key は unsupported/timeout として記録されますが、GNSS 受信は停止しません。

required 設定：

| Key | ID | 目標値 |
|---|---:|---:|
| CFG-SIGNAL-GPS_ENA | `0x1031001F` | 1 |
| CFG-SIGNAL-GPS_L1CA_ENA | `0x10310001` | 1 |
| CFG-SIGNAL-QZSS_ENA | `0x10310024` | 1 |
| CFG-SIGNAL-QZSS_L1S_ENA | `0x10310014` | 1 |
| CFG-UART1OUTPROT-UBX | `0x10740001` | 1 |
| CFG-MSGOUT-UBX_RXM_SFRBX_UART1 | `0x20910232` | 1 |

optional 設定：

| Key | ID | 目標値/単位 |
|---|---:|---|
| CFG-UART1-BAUDRATE | `0x40520001` | 115200。失敗時は検出済み baud rate を維持可能 |
| CFG-UART1OUTPROT-NMEA | `0x10740002` | 既定 0。`KEEP_NMEA_OUTPUT` で 1 に変更可能 |
| CFG-MSGOUT-UBX_NAV_PVT_UART1 | `0x20910007` | 1 |
| CFG-MSGOUT-UBX_TIM_TP_UART1 | `0x2091017E` | 1。未対応時は degraded のみ |
| CFG-TP-PULSE_DEF | `0x20050023` | PERIOD=0 |
| CFG-TP-PULSE_LENGTH_DEF | `0x20050030` | LENGTH=1 |
| CFG-TP-PERIOD_TP1 / LOCK | `0x40050002` / `0x40050003` | 1,000,000 us |
| CFG-TP-LEN_TP1 / LOCK | `0x40050004` / `0x40050005` | 100,000 us |
| CFG-TP-SYNC_GNSS_TP1 | `0x10050008` | 1 |
| CFG-TP-USE_LOCKED_TP1 | `0x10050009` | 1 |
| CFG-TP-ALIGN_TO_TOW_TP1 | `0x1005000A` | 1 |
| CFG-TP-POL_TP1 | `0x1005000B` | 1、整数秒の立ち上がりエッジ |
| CFG-TP-TIMEGRID_TP1 | `0x2005000C` | UTC=0 |
| CFG-TP-TP1_ENA | `0x10050007` | 1 |

VALSET は既定では RAM のみに書き込みます。`CONFIGURE_GNSS_BBR=1` の場合は BBR にも書き込みますが、Flash layer は要求しません。

## QZSS 受信判定と段階的診断

通常環境では 10 秒ごと、`capture-l1s` では 30 秒ごとに、コンパクトな `STATUS` を 1 行出力します。内容には state、UART/UBX/SFRBX、QZSS L1C/A/L1S、PAB/CRC、MT、PPS、TIM-TP、時刻品質、raw drop、および独立した `l1s_generated/queued/sent/dropped` が含まれます。各停止診断は `ADVICE` を 1 回だけ出力し、同じ内容を繰り返し表示しません。すべての異常を「QZSS 受信失敗」と解釈しないでください。

| 現象/診断コード | 意味と確認項目 |
|---|---|
| `uart_bytes=0` / `GNSS_ERR_NO_RX_DATA` | 電源、共通 GND、TX/RX の交差接続、USART2 ピン、候補 baud rate を確認してください。 |
| UART は増えるがプロトコルなし / `GNSS_ERR_RX_DATA_NO_VALID_PROTOCOL` | debug の ASCII/HEX sample を確認してください。現時点では、未知のバイナリ、電気ノイズ、未対応プロトコルを完全には区別できません。 |
| `LikelyWrongBaud` / `GNSS_ERR_LIKELY_WRONG_BAUD` | 候補 baud rate を比較し、3.3 V USB-TTL で検証してください。 |
| 有効な NMEA はあるが MON-VER timeout / `GNSS_ERR_RX_OK_TX_NO_RESPONSE` | RX 経路には強い証拠があります。PA2→モジュール RX、UBX input/port、電圧レベルを確認してください。 |
| `GNSS_ERR_MON_VER_TIMEOUT` | 3 種類すべての baud rate で有効な MON-VER がありません。衛星視野の問題ではありません。 |
| CFG-VALGET NAK | 現在の key が firmware に認識されていません。firmware は VALSET へ fallback します。optional key により受信が停止することはありません。 |
| CFG-VALSET NAK | key、value、firmware version を確認してください。required key が retry を使い切ると degraded になります。 |
| UBX はあるが `sfrbx_total=0` / `GNSS_ERR_NO_SFRBX` | SFRBX MSGOUT が有効になっていない、出力可能な navigation frame がない、または受信条件が悪い可能性があります。 |
| SFRBX はあるが `qzss_sfrbx=0` / `GNSS_ERR_NO_QZSS` | QZSS をまだ追尾できていません。空の見通し、アンテナ、現在地のカバレッジを確認してください。 |
| `qzss_l1ca_sfrbx` は増えるが `qzss_l1s_sfrbx=0` / `GNSS_ERR_NO_L1S` | QZSS は受信していますが、L1C/A または他の信号だけです。QZSS_L1S_ENA を確認してください。 |
| L1S はあるが PAB がすべて無効 | `sigId=1`、numWords/length、raw データの完全性を確認してください。現場ノイズを「修正」するために、検証済みのバイト順を変更しないでください。 |
| PAB は正しいが CRC が誤り | 完全な UBX_RAW を保存してオフライン再生してください。CRC は仕様上の 250-bit 範囲だけを対象とし、末尾 6 bit は padding です。 |
| `unsupported_mt` が増える | 有効ですが、現在は意味解析していない MT を受信しています。raw は保持され、フィールドを捏造しません。 |
| 長時間 MT43/MT44 がない | L1S が動作していないとは限りません。まず `qzss_l1s_sfrbx` が増えているかを確認してください。通報は連続ストリームではありません。 |
| PPS は増えるが UTC が無効 | PPS の電気エッジはありますが、NAV-PVT の日付/時刻/fullyResolved がすべて有効ではありません。 |
| `time=tentative` | 最も可能性が高い直前の PPS は見つかっていますが、NAV-PVT 単独では正確な対応を証明できません。 |
| PPS 入力はあるが TIM-TP が 0 | `CFG-MSGOUT-UBX_TIM_TP_UART1` が verified か、モジュール firmware が対応しているかを確認してください。 |

屋内では通常、GNSS/QZSS L1S を安定して受信できません。屋外で十分な空の見通しを確保し、アンテナの向き、地平線方向の遮蔽、給電線損失、アクティブアンテナへの電源供給に注意してください。災害通報がないことは、L1S が動作していないことを意味しません。

PPS の `pps_lost_total/pps_abnormal_total` は lifetime 累積値であり、復旧後も 0 に戻りません。`pps_consecutive_good` と `pps_activity` は現在の健全性を示します。異常を 1 回検出すると直ちに degraded になり、その後 3 周期連続で正常なら `STABLE` に復帰できます。過去の異常が現在状態を永久に汚染することはありません。1.8 秒を超えて新しいエッジがない場合は、引き続き `TRANSIENT_OR_MISSING` に移行します。

主要統計には、UART/UBX checksum/oversize/resync/NMEA、VALGET/VALSET/ACK/NAK/skip/change、GPS/QZSS/L1C/A/L1S、PAB/CRC/MT/duplicate、PPS historical/current health、NAV-PVT/UTC association、さらに L1S/その他/Critical の段階別 drop、queue high watermark、出力 bytes/s が含まれます。

## raw データ収集

コンパイルスイッチ：

- `RAW_CAPTURE_MODE`
- `RAW_CAPTURE_INCLUDE_ALL_UBX`
- `RAW_CAPTURE_INCLUDE_PRIORITY_UBX`
- `RAW_CAPTURE_INCLUDE_SFRBX`
- `RAW_CAPTURE_INCLUDE_SFRBX_HEADERS`
- `RAW_CAPTURE_INCLUDE_L1S`
- `RAW_CAPTURE_INCLUDE_PPS`
- `RAW_CAPTURE_INCLUDE_TIMTP`
- `RAW_CAPTURE_INCLUDE_TIMTP_MATCH`
- `L1S_COMPACT_CAPTURE`
- `CAPTURE_PRIVACY_MODE`
- `CAPTURE_INCLUDE_NAV_PVT_RAW`

capture/debug 環境は、USART1 から USART2 への backpressure を避けるため、GPS/Galileo/BeiDou の各 SFRBX の完全 raw をすべて保存しません。capture の既定値は `CAPTURE_PRIVACY_MODE=1,CAPTURE_INCLUDE_NAV_PVT_RAW=0` で、緯度経度を含む完全な NAV-PVT packet をログへ書き込みません。debug では明示的な opt-in により NAV-PVT raw を保持でき、起動時に `warning=location_data_present` を表示します。16 進数は常に大文字、空白なし、1 record 1 行です。

```text
UBX_RAW timestamp_us=12345678 len=48 data=B562...
SFRBX_HDR timestamp_us=12345678 gnssId=0 svId=3 sigId=0 freqId=0 numWords=10
SFRBX timestamp_us=12345678 gnssId=5 svId=4 sigId=1 freqId=0 numWords=9 raw=...
L1S timestamp_us=12345678 pps_seq=123 svId=4 pab=C6 mt=44 crc=ok raw=...
QZSS_L1S_UBX timestamp_us=12345678 pps_seq=123 svId=4 numWords=9 pab=C6 mt=44 crc=ok len=52 data=B562...
PPS timestamp_us=12345678 seq=123 interval_us=1000000 pps_lost_total=2 pps_abnormal_total=2 pps_consecutive_good=100
TIMTP timestamp_us=... previous_pps_seq=... tow_ms=... tow_sub_ms=... qerr_ps=... week=... flags=... ref_info=...
TIMTP_MATCH timestamp_us=... pps_seq=... matched=yes ambiguous=no quality=confirmed reason=none
```

`UBX_RAW data` には sync、class、id、little-endian length、payload、CK_A、CK_B が含まれ、完全に再生できます。`SFRBX raw` は UBX payload header より後ろの word byte の元の並びです。`L1S raw` は各 U4 を変換した後の、firmware の BitReader と同じ 32-byte の satellite MSB-first frame です。

`capture-l1s` は、同一メッセージについて `UBX_RAW + SFRBX + L1S` を重複出力せず、`QZSS_L1S_UBX` のみを出力します。この行には完全な 52-byte UBX packet が含まれ、replay による end-to-end 検証が可能です。すべての USART1 テキストは、まず完全な record として構築されてから、固定容量の優先度付きキューに入ります。出力器は `availableForWrite()` に従って分割送信しますが、1 record の改行が完了する前に別の record を挿入しません。優先度は Critical、QZSS-L1S、Config、time-quality、Periodic、その他 Capture、Verbose の順に低下します。6-slot キューのうち 3 slot を L1S 用に予約し、輻輳時は低優先度 record を先に破棄します。USART1 を busy-wait しません。

実到着シーケンスのホスト負荷再生では、115200 8N1 の有効 11,520 byte/s で送出します。コンパクト L1S 1 件の予算は 220 bytes、実測平均は約 510 byte/s です。TIM-TP match と 30 秒ごとの STATUS を加えても十分な余裕があります。2,657 件の実到着タイミングによるテストでは `l1s_records_dropped=0` となり、低優先度 verbose は想定どおり破棄されます。これはホストモデル検証であり、再書き込み後の現場 drop 観測を代替するものではありません。

収集コマンド：

```sh
pio run -e genericSTM32F103C8-capture-l1s -t upload
pio device monitor -b 115200 | tee capture.log
python3 tools/replay_ubx.py capture.log
python3 tools/analyze_capture.py capture.log
```

## PC 側オフライン再生

`tools/replay_ubx.py` は Python 標準ライブラリのみを使用します。ログの 16 進数、宣言長、UBX checksum、SFRBX payload 長、word 数を検証します。`gnssId=5 sigId=1` を抽出し、firmware と同じ U4 バイト順で L1S を復元し、PAB/MT を取り出して CRC を検証します。また、同じ 10 秒ウィンドウ内で衛星をまたぐ重複 frame を検出します。

```sh
python3 tools/replay_ubx.py capture.log
python3 tools/replay_ubx.py capture.log --only-l1s
python3 tools/replay_ubx.py capture.log --export-cpp captured_vectors.h
python3 tools/replay_ubx.py capture.log --export-valid-l1s-cpp valid_l1s_vectors.h
python3 tools/analyze_capture.py capture.log --json report.json
python3 tools/sanitize_capture.py capture.log --strip-nav-pvt --output capture-public.log
python3 tools/import_real_fixtures.py capture.log
python3 tools/import_real_fixtures.py capture.log --check
```

replay の要約には、received/calculated CRC と第 9 word の raw UBX bytes が同時に表示されます。`analyze_capture.py` は完全な UBX を canonical な基準とし、重複集計を避けるため、テキスト `SFRBX_HDR` の受信件数を別項目として報告します。また、先頭ノイズ、firmware 累積値と実保存値、TIM-TP/qErr、privacy risk、JSON も報告します。`sanitize_capture.py` は新しいファイルを atomic に書き込み、入力/出力の SHA-256 を表示し、先頭ノイズを除去します。`--strip-nav-pvt` は、元ファイルや UBX checksum を変更せず、完全な NAV-PVT raw と人間可読の lon/lat も削除します。

## PPS と UTC の関連付けモデル

PA8 ISR は引き続き `micros()`、増加する sequence、flag のみを記録します。メインループは直近 4 件を保持します。

```cpp
struct PpsEvent {
    uint32_t sequence;
    uint32_t microsValue;
    bool utcAssigned;
    int64_t utcSecond;
};
```

各 NAV-PVT について、iTOW、UTC の年月日時分秒、valid flags、nano、受信時の `micros()`、直近の PPS sequence/micros を保存します。`micros()` の差分はすべて unsigned 減算で求め、32-bit wraparound を正しく処理します。

M10 Interface Description では NAV-PVT の `iTOW` を navigation epoch と定義しています。UBX-TIM-TP (`0x0D 0x01`、固定 16 bytes) は、TIMEPULSE 出力の**次の** pulse を記述します。フィールドは little-endian の `towMS U4`、`towSubMS U4`（2^-32 ms）、`qErr I4`（ps）、`week U2`、`flags X1`、`refInfo X1` です。flags から UTC/GNSS time base、UTC availability、RAIM、qErr validity、TP locked を解析します。refInfo には GNSS/UTC reference code を保持します。シリアルメッセージの到着遅延だけでは、特定の電気エッジに対する絶対 UTC を証明できないため、NAV-PVT による関連付けは引き続き tentative のみです。

`confirmed` を生成するには、次の条件をすべて満たす必要があります。PA8 に実際のエッジがあること、有効な TIM-TP がその後の PPS と明確に一致すること、flags が UTC available、TP locked、qErr valid を示すこと、CFG-VALGET で TIMEGRID と TP enable が検証済みであること、sequence/到着ウィンドウに曖昧さがないこと、少なくとも 3 周期連続で week/tow が毎回 1 秒ずつ増加すること、その間に PPS loss または abnormal がないことです。

実際の SPG 5.10 capture では、flags が UTC、locked、曖昧さのない一致を示しましたが、`qErrValid=no` でした。v0.2.4 はこれを `quality=locked_utc_limited reason=qerr_unavailable` と表現し、既存の同期証拠を保持しつつ、qErr の placeholder 0 をゼロ誤差として扱ったり、confirmed に昇格させたりしません。その他の失敗時は `invalid`、`pps_only`、`utc_valid`、`tentative`、`degraded` のいずれかを維持または選択します。

## ソフトウェア構成

- `ubx_parser`：固定 256-byte payload、UBX Fletcher、ノイズ/途中 packet/連続 packet/oversize、NMEA 集計。
- `gnss_config`：移植可能な VALGET/VALSET encode/decode、設定テーブル、差分に基づく判定。
- `max_m10s`：自動 baud rate、MON-VER、ノンブロッキングな設定 read/write/recheck、状態とエラーコード。
- `ubx_dispatcher`：共通 SFRBX 分類、L1S 抽出、統計、重複排除、オフライン注入経路。
- `qzss_l1s`：SFRBX header、U4 変換、安全な BitReader、250-bit、PAB/MT、CRC24、固定重複排除。
- `raw_capture`：完全な UBX/SFRBX/L1S の 1 行形式と、固定容量ノンブロッキング出力。
- `tim_tp` / `time_sync` / `pps_capture`：TIM-TP フィールド、厳密な次 pulse マッチング、4-edge UTC 関連付けモデル、PA8 EXTI backend。
- `gnss_self_test`：VALGET 一致または実メッセージ/エッジ証拠だけに基づく起動 self-test。
- `dcr_dcx_decoder`：伝送層から分離された MT43/MT44 の最小 decoder。
- `output_formatter`：固定バッファによる Human/JSON 出力。
- `tools/replay_ubx.py`：標準ライブラリによるオフライン検証、解析、重複排除、C++ export。
- `tools/analyze_capture.py`：現場 capture の Human/JSON、ファイル単位、衛星系、QZSS、時刻統計。

## プロトコル対応範囲と境界

L1S 伝送層は、検証済みのパラメータを維持します。MSB-first、PAB `0x53/0x9A/0xC6`、MT は bit 8..13 にあります。先頭 226 bit に対して、下位 24-bit polynomial `0x864CFB`、init 0、final xor 0 で計算し、受信 CRC は bit 226..249 にあります。MAX-M10S SPG 5.10 の実 RXM-SFRBX は `numWords=9` を使用します。先頭 8 個の U4 を word ごとに little-endian 変換すると、250-bit L1S + 6 spare bit が得られます。第 9 word は完全な UBX、frame metadata、fixture に保持しますが、L1S フィールドとしては扱いません。2,657 個の実 CRC 一致により、この規則が固定されています。旧 8-word 経路は既存の Switch Science/提供済み DCX ベクトルのためだけに残しています。10 word や、広すぎる `>=8` 条件を L1S として受け入れません。

旧 default/JSON firmware には、PC なしでの診断用として、standalone-lite レベルの Report Classification、Disaster Category、少数の CAMF raw code を残しています。これは full decoder ではありません。正式な Python 層では、DCR-016 に従って 12 個の有効カテゴリを解析し、DCX-004/CAMF 1.2 に従って A1–A18、A4 1–113、A17/A18 specific settings、EX1–EX10 を解析します。EX11 と、未監査の最新 JIS 名称データは、support matrix に従って raw/partial のまま保持し、推測しません。

`USE_TIM1_INPUT_CAPTURE` は引き続き予約スイッチです。v0.2.4 では PA8 EXTI をベースラインとして使用します。

## テスト

native は 139 項を維持しています。Python package は 93 項、tools は 83 項で、そのうち 4 項は optional external input がないため skip されます。現在の v0.2.7 回帰には、24 時間 bucket、STATUS 複数ウィンドウの閉合、3 種類の出力ストリーム accounting、rotation/health/disk guard、実 HGRAW corruption、semantic fingerprint v2、Marine/Null/SLAS、bounded repeat state、DCR 7 シナリオ/9 カテゴリ、南海トラフ 27 ページ、降灰 5 通/18 地点、not-received 境界、および実 J-Alert EX9 64-bit fixture と逐 frame semantic-error 隔離が含まれます。

ホスト側 ASan/UBSan は `sh tools/run_native_sanitizers.sh` で実行できます。これは STM32 実機が sanitizer を通過したことを意味せず、すべての業務 enum 組み合わせについて公式バイナリベクトルが存在することも意味しません。

## 起動 self-test と収集 metadata

初期化が終端状態に入ると、固定形式の `HoshiGuard GNSS Self Test` レポートを表示します。`GNSS RX activity`、`GNSS TX attempted`、`Bidirectional UBX communication` は別々に表示されます。双方向通信が PASS になるのは MON-VER response を受信した場合だけです。UBX/NMEA の証拠がない場合は `Detected baud: UNKNOWN` とし、別項目で `Last attempted baud` を表示します。QZSS/L1S/protocol/MSGOUT の PASS は、目標値と一致した CFG-VALGET のみを根拠とします。VALSET ACK だけでは verified になりません。

PPS の最初のエッジは `OBSERVED_ONCE` のみを表示します。直近で 3 回以上連続したエッジがあり、lost/abnormal がない場合に `STABLE` となります。単一エッジの後 1.8 秒を超えて次のエッジがない場合は `TRANSIENT_OR_MISSING` を表示します。optional TIMEPULSE/TIM-TP が未対応の場合、overall は DEGRADED になりますが、QZSS L1S の受信は停止しません。

ファイル先頭の `META` には、firmware version、build date/time、任意の Git commit、RAM/BBR layer、ユーザーがビルド時に指定した antenna/region/sky が含まれます。`META_PRIVACY` は privacy/nav_pvt_raw を明示し、最初に有効な UTC を得た時点で `META test_start_utc=...` を別途出力します。`OUTPUT_NAV_PVT_COORDINATES` を無効にするだけでは NAV-PVT raw から位置を復元できるため、capture/capture-l1s では既定で NAV-PVT raw も無効にします。ログを提出する前には、引き続き sanitizer を実行し、目視確認してください。

META とすべての `snprintf` ログは共通して `safeSnprintf()` を通ります。負の戻り値は送信せず、切り詰め時の長さを `capacity - 1` に制限し、末尾 byte を強制的に `\0` にします。`STATUS` の `format_truncated` / `format_errors` により、現場での書式異常を検出できます。META の format に失敗しても、起動シーケンスは PPS と GNSS の初期化を継続します。

STM32duino Core 2.4.0 では、同一インスタンスに対して `HardwareSerial.begin()` より前に `end()` を呼んではなりません。未初期化の `serial_t::index` は 0 であり、`UART1_INDEX` と解釈されます。HoshiGuard は USART2 がすでに起動済みかを記録します。最初の baud rate 設定では `begin()` だけを呼び、その後、実際に baud rate を切り替える場合のみ `end()` / `begin()` を実行します。正常起動時は、META の後に次の行が続きます。

```text
GNSS_STATE from=Idle to=DetectingBaud reason=startup ...
GNSS_STATE from=DetectingBaud to=PollingMonVer reason=MON-VER poll ...
```

## Baud switch の診断

実データ `capture-uart-v022.log` により、MAX-M10S ROM/FW SPG 5.10、PROTVER 34.10 が 38400 で有効な UBX-MON-VER を双方向送受信できることを確認しています。poll は `B5620A0400000E34` です。旧手順では `CFG-UART1-BAUDRATE` を通常キューの先頭に置き、旧 baud rate で ACK を待ってから STM32 を切り替えていました。しかし、モジュールは実際には直ちに 115200 へ切り替わるため、ホストは 38400 のまま、すでに解読できない ACK を待ち続け、その後の CFG も誤った baud rate で送信していました。

v0.2.3 の手順は次のとおりです。MON-VER を検出・取得 → すべての通常 required/optional 設定を完了・再検証 → baud VALSET を単独送信 → 旧 baud rate で 100 ms 静止 → `end()/begin(target)` → UBX/NMEA/probe の途中状態を reset → 新 baud rate で 100 ms 静止 → MON-VER を poll。MAX-M10S Integration Manual 4.2.2 は、baud rate 変更後、通常約 100 ms 待ってから入力することを要求しています。2 回目の 100 ms は、ホスト再起動とポート安定化のための保守的なウィンドウです。ACK は処理証拠としてのみ記録し、checksum が正しい MON-VER を受信した場合にだけ `VERIFIED` とします。

新 baud rate で timeout した場合は、target、original の順に探査し、その後 9600/38400/115200 を有限回だけ確認します。original が応答した場合は `KEPT_ORIGINAL` と表示し、通常設定の証拠を保持したまま SFRBX 検証を継続します。overall は READY/DEGRADED となり、optional baud の失敗だけで FAILED にはなりません。有限候補すべてが失敗した場合にのみ `GNSS_ERR_BAUD_SWITCH_LOST_COMMUNICATION` を報告します。self-test には次も表示されます。

```text
Initial baud: 38400
Target baud: 115200
Verified baud: 115200/38400/UNKNOWN
Baud switch: VERIFIED/NOT_NEEDED/DISABLED/KEPT_ORIGINAL/FAILED
```

最初に `genericSTM32F103C8-debug-38400` を書き込むことで、baud rate を切り替えない通常 CFG 手順を検証できます。その後 debug を使用して、115200 への安全な切り替えを確認します。ACK、`Serial.begin()`、RX activity だけでは verified baud を設定できません。

## v0.2.4 実機現場手順

1. 使用するキャリア基板の給電方式に適したアクティブ/パッシブ GNSS アンテナを使用し、屋外、窓外、または空が遮られない場所に設置します。アンテナの向き、地平線方向の遮蔽、給電線損失に注意してください。
2. `genericSTM32F103C8-debug`（初回診断）または `genericSTM32F103C8-capture-l1s`（長時間・低損失収集）を書き込みます。
3. 20～30 分間の完全なログを保存します。

   ```sh
   pio run -e genericSTM32F103C8-capture-l1s -t upload
   pio device monitor -b 115200 | tee capture.log
   ```

4. MON-VER、self-test、CFG 行を保存します。最初に `uart/ubx`、次に `sfrbx`、QZSS、`sigId=1` を確認してください。MT43/44 がない場合でも、その他の PAB/CRC が有効な MT を確認します。
5. 独立解析を実行します。

   ```sh
   python3 tools/replay_ubx.py capture.log --only-l1s
   python3 tools/analyze_capture.py capture.log --json report.json
   python3 tools/sanitize_capture.py capture.log --strip-nav-pvt --output capture-public.log
   python3 tools/import_real_fixtures.py capture.log
   python3 tools/import_real_fixtures.py capture.log --check
   ```

`capture-outdoor.log` により、v0.2.3 の 115200 双方向 UBX、通常設定、継続的 SFRBX、QZSS L1C/A/L1S、PAB/CRC、TIM-TP/PPS マッチング、3D fix を実機で検証済みです。v0.2.4 の新しいキュー予約ポリシー、PPS 復旧表示、limited 文字列、NAV-PVT privacy 既定値については、再度 firmware を書き込んで現場確認する必要があります。ホストテストは、実機の長時間安定性や PPS 精度校正と同等ではありません。一定時間内に実災害通報が必ず出現する保証もありません。

## 参照資料

- [Switch Science 元記事](https://www.switch-science.com/blogs/magazine/gps-qzss-dc-report-dcx-receiving)
- [Switch Science samplecodes](https://github.com/SWITCHSCIENCE/samplecodes/tree/master/QZSS_DC_Report_DCX)
- [u-blox M10 SPG 5.20 Interface Description](https://content.u-blox.com/sites/default/files/documents/u-blox-M10-SPG-5.20_InterfaceDescription_UBXDOC-304424225-20128.pdf)
- [MAX-M10S Integration Manual](https://content.u-blox.com/sites/default/files/MAX-M10S_IntegrationManual_UBX-20053088.pdf)
- [QZSS 公式 PS/IS ページ](https://qzss.go.jp/en/technical/ps-is-qzss/ps-is-qzss.html)：IS-QZSS-L1S、IS-QZSS-DCR-016、IS-QZSS-DCX-004
