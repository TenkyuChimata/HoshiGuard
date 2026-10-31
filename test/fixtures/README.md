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
pio run -e genericSTM32F103C8-bridge-binary -e generic-minimal -e explorer-e-c111g -e explorer-e-c111g-recovery
pio run -e generic-tim1-validation -e generic-iwdg-validation -e generic-tim1-iwdg-validation
pio run -e bluepill-tim1-validation -e explorer-e-c111g-full
pio run -e explorer-e-c111g-72mhz -e explorer-e-c111g-72mhz-tim1
pio run -e explorer-e-c111g-72mhz-iwdg -e explorer-e-c111g-72mhz-tim1-iwdg
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
| `test/test_tim1_capture.cpp` | 製品と共通の CCR 読み出し・競合判定、UIF の非依存性、下位間隔・通し番号周回。レジスター境界は模擬 |
| `tools/tests/firmware_sim/` | Arduino/HAL 境界の模擬で実際の setup/loop を実行。復旧後の設定検証・PPS/TIM-TP、実 UBX の再受信、Bridge 全フレーム検査、OLED 故障・抑止、無効 GPIO。実機試験ではない |
| `python/tests/test_{transport,decoders,events,api,operations}.py` | ホスト検証、DCR-017/DCX-004、意味識別・状態、HTTP/WS・再起動、現行 diagnose |
| `tools/tests/test_{capture,sessions,specification}.py` | 捕捉・秘匿化・生成器、実証跡・合成 STATUS 境界、仕様・公式期待値 |
| `test/test_support.h` / `python/tests/support.py` | 共通入力の生成・読み込み。独立したテスト入口は持たない |

同じ動作の断言は表駆動テストや `subTest` にまとめます。8/9 word、text capture、診断 NDJSON は
現在も実装が受理する入力なので、その検証を維持します。過去の修正に由来する検証も機能別に配置します。

### TIM1/IWDG のソフトウェア検証と実機境界

`pio test -e native` と sanitizer は `tim1_capture.h` の製品コードを直接実行し、
捕捉単体、連続 1 Hz、短間隔、欠落後の長間隔、下位カウント周回、UIF 同時発生・未処理、
多数の連続周回、overcapture、CCR 読み出し競合と ES096 の EXTI pending、間隔証跡の無効化を確認します。
72 MHz の各位相、1/2/10 秒、±100 ppm の tick 数、32/64 bit 境界でも下位値しか得られないこと、
HPRE/PPRE2 の全符号化も検査します。完全時刻・秒間隔の実装を検証したという意味ではありません。
秒間隔への周回数の復元は実装していないため、UIF から復元できたとする模擬試験は設けません。
整数 ns への変換や高位カウンターも追加しておらず、それらの丸め・単調性保証は対象外です。

`tools/run_hardware_simulation.sh` は実際の `setup()/loop()`、`PpsCapture`、GNSS 設定・復旧、
TIM-TP/Bridge を、Arduino とレジスターの境界だけ差し替えて実行します。
TIM1/IWDG の 4 組合せで同じ合成入力・実 UBX fixture を使い、全 Bridge wire byte を `cmp` します。
ビルド時刻による HELLO の差を避けるため `SOURCE_DATE_EPOCH` を固定します。
再初期化、`micros()` の 32 bit 周回、主ループの取りこぼし、ハード復旧後の古い証跡の破棄、
TIM1 の所有・既存クロック・remap・72 MHz 不成立による EXTI 継続、APB2 タイマークロック 2 倍則も検査します。
72 MHz の PSC=0/ARR=65535/RCR=0、HardwareTimer setup が有効にする NVIC の明示的な無効化も確認します。
8/48/64 MHz、AHB/APB 分周、古い HAL クロック、PLL/HSE 未準備、クロック切替中を拒否します。
実行中の 22 種類の設定・所有・gate/reset/remap/NVIC 故障を製品コードへ注入し、
証跡無効化、他者の設定の保存、故障ラッチ、明示再初期化、二つの新規捕捉による間隔証跡の復帰を確認します。
既存の `test_time.cpp` による NAV-PVT、TIM-TP、UTC の confirmed/tentative 回帰も維持します。

IWDG は導入済み STM32duino の **実際の `IWatchdog.cpp`** をコンパイルし、LL 呼び出しだけを模擬します。
公称 24 秒の PR/RLR、全 loop 完了ごとのリロード、長い GNSS 探索・設定再試行、ソフト復旧・RESET_N 復旧、
連続 UART ノイズ、Bridge 背圧、OLED の NACK、ISR だけが動く主ループ停止、受信・送信・OLED 処理途中の停滞注入を検査します。
停滞注入は境界から例外を投げてその回のリロードが行われないことを検査し、MCU の実際のデッドロック・リセットは再現しません。
無効時は TIM1 初期化と IWDG 起動・リロードがなく、不正スイッチ・未対応 MCU はコンパイルで拒否されます。
復帰原因診断は実装していないため、原因分類の成功は主張せず、起動時のフラグに触れないことを検査します。
IWatchdog の検索先は `${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoststm32` です。
上記の firmware build を先に実行してフレームワークを導入してください。

同スクリプトは clock profile 0/1/2 も検査します。実際の generic variant と strong な SystemClock_Config を
同時にリンクし、従来の HSI 48 MHz、8 MHz 水晶 ON、8 MHz 外部発振器 Bypass の選択を確認します。
RCC/Flash/SysTick、SystemCoreClock と HAL 読戻しの一致・不一致、HSE/PLL/HAL の起動失敗、
CSS/NMI と実行中の設定異常による安全停止を検査します。停止時の __WFI は模擬境界で例外として捕捉します。
CPU が実際に停止・リセットした証拠ではありません。故障後に UART/TIM1/I2C/SysTick が停止し、
製品 loop の時刻処理や送信へ進まないことを断言します。

core の実 clock.c による micros/millis/SysTick、導入済み HAL の実 BRR マクロ、
製品 oled_stm32.cpp の I2C 初期化も 48/72 MHz で実行します。HAL RCC の待機ループ・発振器・バスの電気動作は模擬です。
72 MHz の各 profile と TIM1/IWDG の 4 組合せは、旧 48 MHz と同じ実 UBX/合成 PPS 入力で全 Bridge byte を比較します。
profile 不正値・未対応 MCU・8 MHz 以外の HSE_VALUE は拒否します。

標準の三つの `generic-*-validation` に加え、最小構成・Explorer・復旧環境でも同じ二つのフラグを組み合わせてビルドします。
これらはホスト上のソフトウェア検証であり、以下は実機試験として残ります。

- PA8 の実 PPS と信号発生器を使った CCR1 のラッチ、周回近傍、短パルス・欠落・復帰、overcapture の確認。
- `F_CPU` と実 RCC を区別し、72 MHz の実周波数・PSC=0・約 910.222 µs 周回を確認する。
- 外部発振器と無源水晶のモード、Flash、SysTick、UART BRR と線上 baud、I2C SCL を実機で確認する。
- HSE 切断で CSS/NMI が働き、通信停止・SWD 診断・IWDG または手動リセット後の再初期化を確認する。
- EXTI の既存時刻と CCR1 の位相、同期回路・入力波形・発振器誤差を含むジッター測定。UTC 誤差は別に評価する。
- 高負荷の GNSS UART と Bridge を同時に動かし、ISR 時間、USART2 ORE、受信欠落、Bridge の遅延と最大 loop 時間を比較する。
- IWDG を有効にした実 MCU で主ループを停止し、LSI の条件差を含めて実際のリセットと再起動・受信再開を確認する。
- GNSS 無信号、設定失敗、OLED/I2C 故障、GNSS の正常な復旧、ホスト切断で誤リセットしない長期試験。
- 電源投入・再初期化・SWD ブレークポイント、option bytes、通常基板と Explorer の両方での動作確認。

実機検証が済むまで両機能の本番推奨は無効です。ビルド成功や sanitizer の成功を実機試験完了と扱いません。
方式比較、generic variant の実クロック、Blue Pill の基板条件、変更前後の容量と最新の件数は
[TIM1 審査記録](../../docs/tim1-pps-review.md)を参照してください。

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
