# テストと実受信データ

## 検証コマンド

リポジトリのルートで実行します。Python テストには loopback socket の利用権限が必要です。

```sh
pip install './python[test]'
python3 -m unittest discover -s python/tests -v
python3 -m unittest discover -s tools/tests -v
python3 tools/check_spec_inventory.py
python3 tools/check_alert_support_matrix.py
python3 tools/check_alert_traceability.py
pio test -e native
sh tools/run_native_sanitizers.sh
sh tools/run_hardware_simulation.sh
```

sanitizer スクリプトは PlatformIO が取得した Unity を使います。AddressSanitizer/UndefinedBehaviorSanitizer を有効にし、
管理実行環境の ptrace 制約により LeakSanitizer は無効にします。
標準テストは同梱データだけで実行し、外部原資料の有無による skip は設けません。
未公開の完全ログとの照合は後述の `--check`、原 PDF の照合は仕様一覧の SHA-256 を使います。
デコーダーの存在、合成試験、実放送による検証を区別し、詳細は[対応範囲](../../docs/protocol.md#対応範囲と証拠の強さ)を参照してください。

## テストの配置

| 場所 | 対象 |
|---|---|
| `test/test_{transport,gnss,time,logging,bridge}.cpp` | UBX/L1S/CRC、GNSS 設定・復旧、PPS/TIM-TP、書式・キュー、Bridge 1.0 |
| `test/test_oled.cpp` | I2C の各送信段階・総期限、SSD1306 の画面更新・障害降格、状態の鮮度・文字境界、実 UBX と Bridge の併用。ハードウェア試験ではない |
| `test/test_receiver_aux.cpp` | 状態 LED、起動 OLED スイッチ、起動専用復旧の期限・停止条件・周回・回数制限 |
| `tools/tests/firmware_sim/` | Arduino/HAL 境界の模擬で実際の setup/loop を実行。復旧後の設定検証・PPS/TIM-TP、実 UBX の再受信、Bridge 全フレーム検査、OLED 故障・抑止、無効 GPIO。実機試験ではない |
| `python/tests/test_{transport,decoders,events,api,operations}.py` | ホスト検証、DCR-017/DCX-004、意味識別・状態、HTTP/WS・再起動、現行 diagnose |
| `tools/tests/test_{capture,sessions,specification}.py` | 捕捉・秘匿化・生成器、実証跡・合成 STATUS 境界、仕様・公式期待値 |
| `test/test_support.h` / `python/tests/support.py` | 共通入力の生成・読み込み。独立したテスト入口は持たない |

同じ動作の断言は表駆動テストや `subTest` にまとめます。8/9 word、text capture、診断 NDJSON は
現在も実装が受理する入力なので、その検証を維持します。過去の修正に由来する検証も機能別に配置します。

## 同梱する証跡

| ディレクトリ | 出所・用途 |
|---|---|
| [real/manifest.json](real/manifest.json)・[packets.h](packets.h) | 2026-07-14、MAX-M10S SPG 5.10 の実受信から選んだ完全な RXM-SFRBX。合成負例は manifest で区別 |
| [bridge_real_session/manifest.json](bridge_real_session/manifest.json) | 実機の約 1 時間の HGRAW から選んだ Bridge wire frame と PC 受信時刻 |
| [bridge_24h_session/manifest.json](bridge_24h_session/manifest.json) | ファームウェア 0.2.5 の約 24 時間の HGRAW 抜粋 |
| [official/dcr-scenario.json](official/dcr-scenario.json) | 公式試験計画の期待値。実受信バイナリではない |
| [official/dcx-scenario.json](official/dcx-scenario.json) | DCX 公式試験計画の期待値。ビット仕様ではない |

合成した DCR/DCX 境界値、STATUS カウンター、不正入力、Mock は各テスト内で生成し、別ファイルへ重複保存しません。
`packets.h` は元の 14 packet をまとめたもので、manifest のバイト列・ハッシュおよび生成器の出力と照合します。
`real/real_capture_arrivals.h` は実受信間隔と PPS の派生データで、MCU のキュー負荷・時刻回復試験に使います。

元の完全な現場ログは同梱していません。公開 fixture は NAV-PVT、座標、正確な所在地、個人識別情報、USB シリアル番号を保持しません。
`region: withheld` と `coordinates: not retained` は意図的な情報非保持です。
`real/` の選択規則は、対象 MT/PAB ごとに時刻順で最初の checksum/PAB/CRC が正しい完全な UBX レコードです。
manifest に元ログ SHA-256、受信時刻、svId/sigId/numWords、PAB/MT、CRC、重複判定、packet hash を残します。
この fixture 群の基本証拠は伝送・PAB・MT・CRC であり、全カテゴリの意味を手作業で検証したものではありません。

`qzss_l1ca_negative` は実受信です。non-QZSS、checksum-corrupted、SFRBX-length-mismatch、および
[negative_vectors.json](bridge_real_session/negative_vectors.json) は合成・改変した負例です。
実際の衛星フレームや受信機故障と説明しないでください。

Bridge 抜粋は HELLO/STATUS/TIME_STATUS、観測 MT/PAB/衛星、両端を含み、24 時間版では各時刻帯、
キュー最大使用量の変化、DCR/DCX 変種、最大 L1S 間隔の境界も含みます。
元セッションの SHA-256 は各 manifest、全体集計は各 expected_summary.json に記録します。
0.2.5 の証跡は Bridge 1.0 と変更のない経路を評価する資料であり、現在のファームウェアの 24 時間実機試験完了を意味しません。
抜粋に伴う sequence の飛びを、元セッションの実際の欠落と解釈しないでください。

## 再生成とシナリオ検証

対応する元ログを用意した場合の、決定的な再生成・照合コマンドです。

```sh
python3 tools/import_real_fixtures.py capture-outdoor.log --check
python3 tools/import_bridge_session_fixture.py captures/v025-session.hgraw --check
python3 tools/import_24h_session_fixture.py captures/soak-24h.hgraw --check
```

`--check` を外すとテスト用データを再生成します。生成物の仕様説明は本書に集約しています。
公式試験計画の PDF は同梱せず、[spec-inventory.json](../../docs/spec-inventory.json) に原ファイル名・SHA-256・公式案内ページを残します。
案内ページは更新されるため、別月の PDF を 2026 年 7 月の資料として扱わないでください。
DCR の元 PDF を取得した場合は、次のように原ファイルのハッシュも照合できます。

```sh
sha256sum /path/to/details_202607.pdf
# official/dcr-scenario.json の oracle.sha256 と一致することを確認
```

標準テストでは manifest と仕様一覧の整合性、同梱抜粋からの決定的な再抽出、合成ログの生成・改変拒否を検証します。
STATUS の合成試験は、取得前の累積値、スナップショット時の未送信分、実際のカウンター不一致を区別します。
完全な原ログから同じ抜粋を選べること、現行ファームウェアの実機長時間安定性、原 PDF の内容の再監査は別途確認が必要です。

```sh
python3 tools/validate_dcr_test_scenario.py captures/dcr-202607.hgraw --manifest test/fixtures/official/dcr-scenario.json
python3 tools/validate_dcx_test_scenario.py captures/dcx-202607.hgraw --manifest test/fixtures/official/dcx-scenario.json
```

DCR manifest は Rc=7、2 回の JST/UTC 配信窓、7 シナリオ・9 カテゴリ、南海トラフ 27 ページ、
降灰 5 通・18 地点、津波と洪水の action を記述します。`not_received`、デコード失敗、フィールド不一致、正常を区別します。
優先する正式情報による中断はあり得るため、未受信をデコーダー故障とみなしません。

## 公開 JSON 例

例は現在のデコード処理による PublicAlert です。形式確認だけでなく、同じ HGRAW レコードからの再生成一致も検証します。

| JSON | HGRAW | レコードのオフセット |
|---|---|---:|
| [DCR Marine](../../docs/public-alert-real-marine.json) | [bridge_real_session/selected_records.hgraw](bridge_real_session/selected_records.hgraw) | 220 |
| [DCX L-Alert](../../docs/public-alert-real-dcx.json) | [bridge_24h_session/selected_records.hgraw](bridge_24h_session/selected_records.hgraw) | 4608 |

抜粋 HGRAW の SHA-256 は順に `74b27282f1438cc8e4080f771b36e93ed8bc635cb01d65a632220dcf9575eaa9`、
`e5cb12abb3295f981bbc39557a3dbd670eabff51d9eacf341e1cbdc19cdb49ed` です。
Marine は DCR14 の海上濃霧警報、DCX は L-Alert の訓練・試験メッセージです。
DCX 例を正式なミサイル警報や実避難イベントの受信証拠として扱わないでください。
`input=replay` は読み取り方法を示し、`times.received` は元の現場 PC 時刻です。
API のフィールドを含む実出力は証跡としてそのまま保持し、説明文のみを日本語に統一します。

## オフラインツール

すべて `tools/` 内のスクリプトです。引数は各 `--help` を参照してください。

| ツール | 用途 |
|---|---|
| analyze_capture | 捕捉データの分析 |
| validate_bridge_session / analyze_soak_session / audit_ndjson_stream | セッションの整合性・長時間証跡・出力の集計 |
| import_real_fixtures / import_bridge_session_fixture / import_24h_session_fixture | 実証跡から公開可能な回帰データを抽出 |
| validate_dcr_test_scenario / validate_dcx_test_scenario | 公式計画と受信内容の比較 |
| sanitize_capture / capture_io / replay_ubx | プライバシー処理・取得・UBX 再生 |
| check_spec_inventory / check_alert_support_matrix / check_alert_traceability | 仕様一覧・対応範囲・フィールド対応の検証 |
| benchmark_api / run_native_sanitizers | 現行経路のソフトウェア性能・ネイティブ検証 |

Bridge のキュー負荷は実装そのものを使う C++ テストで検証します。
`benchmark_api` は現行経路のみを対象とし、`--frames`、`--unique`、任意の `--network` を指定できます。
ベンチマーク結果はハードウェア性能や長時間運転の認証ではありません。
