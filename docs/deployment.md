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

## 追加ハードウェアとビルド環境

HoshiGuard は汎用の STM32F103C8 + MAX-M10S 受信機です。最小配線は README の USART2、PPS、
USART1、共通 GND と電源・アンテナです。追加回路を接続しなくても起動・受信できます。
既存の `genericSTM32F103C8-bridge-binary` と既定環境は変更せず、OLED 有効を維持します。

| 環境 | OLED | PC13 吸電流 LED | 起動 OLED スイッチ | PA15 復旧 |
|---|---|---|---|---|
| `genericSTM32F103C8-bridge-binary`（従来の既定） | 有効 | 無効 | 無効 | 無効 |
| `generic-minimal` / `explorer-e-c111g-minimal` | 無効 | 無効 | 無効 | 無効 |
| `generic-status-led` | 無効 | 有効 | 無効 | 無効 |
| `explorer-e-c111g` | 有効 | 無効 | 有効 | 無効 |
| `explorer-e-c111g-no-oled` | 無効 | 無効 | 動作なし | 無効 |
| `explorer-e-c111g-recovery` | 有効 | 無効 | 有効 | 有効 |

```sh
pio run -e generic-minimal
pio run -e explorer-e-c111g
# RESET_N の実配線・電圧条件を確認した場合だけ選択
pio run -e explorer-e-c111g-recovery
```

独自環境では `build_flags` の `-D HOSHIGUARD_OLED=0/1`、`HOSHIGUARD_STATUS_LED=0/1`、
`HOSHIGUARD_OLED_SWITCH=0/1`、`HOSHIGUARD_GNSS_RESET=0/1` をそれぞれ指定します。
`0/1` は選択肢の表記で、実際には `-D HOSHIGUARD_GNSS_RESET=0` のように一方を記述します。
OLED 以外は未指定なら 0 です。継承元ですでに指定されているフラグは置き換え、同じ名前を重複指定しないでください。
OLED 無効時は表示スイッチも読まず、I2C1 を初期化しません。LED・復旧の無効時も対応する状態・GPIO 操作を生成しません。
GPIO は `board_support.cpp`、時刻依存の補助処理は `receiver_aux.cpp` に分離し、Bridge 1.0 と UART baud は変更しません。

### 任意の 72 MHz システムクロック

既定環境は従来の variant 初期化を維持します。今回の generic ビルドは HSI/2×12 の **48 MHz** です。
STM32duino は `F_CPU` を通常 `SystemCoreClock` として扱い、HAL は実際の PCLK から UART を設定します。
板定義の 72 MHz 表示だけを根拠に動作周波数を判断しません。

`HOSHIGUARD_CLOCK_PROFILE` は `0`（従来、既定）、`1`（8 MHz 無源水晶、HSE ON）、
`2`（8 MHz 外部発振器、HSE Bypass）です。TIM1、IWDG、OLED、スイッチ、RESET の有効化とは独立しています。
`1/2` だけが STM32duino の weak `SystemClock_Config()` を HAL ベースの実装で置換します。
HSE_VALUE=8,000,000、PLL×9、SYSCLK/HCLK=72 MHz、PCLK1=36 MHz、PCLK2/TIM1CLK=72 MHz、
Flash latency=2、prefetch 有効、USB=48 MHz、ADC=12 MHz を設定・照合します。
HAL の `ClockConfig` に SystemCoreClock と 1 ms SysTick の更新を任せます。

| 環境 | 発振器 | TIM1 証跡 | IWDG |
|---|---|---|---|
| 既存の generic/Explorer 環境 | 従来の variant | 既存スイッチのまま。48/64 MHz なら安全拒否 | 既存スイッチのまま |
| `bluepill-tim1-validation` | 8 MHz 無源水晶、ON | 有効 | 無効 |
| `explorer-e-c111g-72mhz` | 8 MHz 外部発振器、Bypass | 無効 | 無効 |
| `explorer-e-c111g-72mhz-tim1` | 同上 | 有効 | 無効 |
| `explorer-e-c111g-72mhz-iwdg` | 同上 | 無効 | 有効 |
| `explorer-e-c111g-72mhz-tim1-iwdg` | 同上 | 有効 | 有効 |

Explorer E-C111G PCB 20250613 の Y1 は SG-8002CA 8M-PCML0 の CMOS 発振器で、OSC_IN/PD0 を駆動します。
参照 PDF の直列抵抗は **R23=10 Ω**、PD1/OSC_OUT は未接続です（R22 は RS485 終端）。
RM0008 §7.2.1 の外部クロック接続なので HSEBYP=1 を選びます。Explorer 原版の `RCC_HSE_ON` をそのままコピーしません。
無源水晶は OSC_IN/OSC_OUT 間の発振回路を必要とするので Bypass と混同しません。
汎用基板も実際の発振器に合う profile を選べます。EEPROM や Explorer の周辺機能を前提としません。

起動失敗、PLL/HSE 未準備、RCC/HAL/Flash/SysTick 読戻し不一致は、UART 初期化・受信処理に進まず安全停止します。
通常動作でも setup/loop の入口で設定を検査します。CSS は有効にし、HSE 故障の NMI では CSSC を消去した上で
UART1/2、TIM1、I2C1、SysTick を停止します。HSI への自動切替後に古い BRR/時基で通信を継続しません。
`board_clock::lastFault` を SWD から読めます。故障理由は RAM のみで、リセット後は保持しません。
既に動作中の IWDG があれば給餌せず LSI によるリセットに任せ、それ以外は SWD/外部リセットまで意図的に停止します。
安全停止中は回復待ちの UART ログや時刻推定を行いません。IWDG の 24 秒公称値を CPU 周波数比で変更しません。

実機検証前の通常運用は従来環境を使います。次は **操作者が手動で実行する例**で、この改修では upload を実行していません。

```sh
# 従来の通常運用用。追加機能・クロック設定は既定値のまま。
pio run -e genericSTM32F103C8-bridge-binary
pio run -e genericSTM32F103C8-bridge-binary -t upload

# Explorer の 8 MHz 外部発振器を確認した実機評価用。
pio run -e explorer-e-c111g-72mhz
pio run -e explorer-e-c111g-72mhz-tim1
pio run -e explorer-e-c111g-72mhz-tim1 -t upload
# IWDG を合わせる評価には explorer-e-c111g-72mhz-tim1-iwdg を選ぶ。
```

実際の 72 MHz、UART/I2C 波形、PPS ジッター、HSE 切断と IWDG 復帰、消費電力、長期連続運転は未測定です。
新環境の本番推奨は実機試験後に判断します。[審査記録](tim1-pps-review.md)に根拠と検証境界をまとめています。

### 任意の TIM1 PPS 証跡と IWDG

STM32F103C8T6 と MAX-M10S のみで使用できる実機検証用の機能です。Explorer の回路は使いません。
`hardware_options.h` の `USE_TIM1_INPUT_CAPTURE` と `HOSHIGUARD_IWDG` をそれぞれ `0` / `1` で指定します。
未指定は両方とも `0` で、既存の全環境はその既定値を維持します。TIM1 用の別名スイッチは設けません。

| ビルド例 | TIM1 証跡 | IWDG |
|---|---:|---:|
| `genericSTM32F103C8-bridge-binary` / `generic-minimal` | 0 | 0 |
| `generic-tim1-validation` | 1 | 0 |
| `generic-iwdg-validation` | 0 | 1 |
| `generic-tim1-iwdg-validation` | 1 | 1 |

```sh
pio run -e generic-tim1-validation
pio run -e generic-iwdg-validation
pio run -e generic-tim1-iwdg-validation
```

これら三つは既存の Bridge Binary + OLED 環境を継承します。OLED は必須ではありません。
独自環境では `generic-minimal`、`explorer-e-c111g`、`explorer-e-c111g-recovery` などを継承し、
その `build_flags` に `-D USE_TIM1_INPUT_CAPTURE=1`、`-D HOSHIGUARD_IWDG=1` を必要な分だけ追加します。
無効化は各フラグを `0` に戻すか削除して再ビルドします。同一フラグを重複指定しないでください。
不正なスイッチ値と未対応 MCU はコンパイル時に拒否します。切替はビルド時のみです。

#### TIM1 の動作と限界

`USE_TIM1_INPUT_CAPTURE=1` は PA8/TIM1_CH1 の補助 CCR1 証跡を有効にします。
RCC から得た TIM1 入力が公称 **72 MHz** である場合だけ PSC=0、ARR=65535、RCR=0 を設定します。
1 tick は約 **13.888889 ns**、16 bit 周回は約 **910.222222 µs** です。
それ以外のクロック、古い `SystemCoreClock`、所有・remap 競合は安全に拒否し、元の EXTI を続けます。

**`genericSTM32F103C8` の名前や `F_CPU=72000000` は 72 MHz の動作証明ではありません。**
今回使用した STM32duino 2.4.0 は generic variant の `generic_clock.c` を選びます。
現在の `PIO_FRAMEWORK_ARDUINO_ENABLE_CDC=0` も builder が定義名の存在で処理するため `USBCON` が有効となり、
実際の基線 ELF は HSI/2×12=48 MHz の初期化を含んでいました。USB 定義がなければ同 variant は HSI/2×16=64 MHz です。
既存の generic/Explorer 環境のシステムクロックや USB 設定は今回変更せず、これらの構成では TIM1 証跡だけ無効になります。

外付け **8 MHz 水晶を実装した Blue Pill** では、次の専用検証環境が clock profile=1 の HAL 実装で
HSE ON×9、HCLK=72 MHz、PCLK1=36 MHz、PCLK2=72 MHz を選びます。
同じ入力捕捉スイッチを継承します。clock profile=1 の標準 HAL 初期化と安全検査も使用します。

```sh
pio run -e bluepill-tim1-validation
```

この board 定義を水晶のない基板へ流用しません。8 MHz 外部発振器があれば profile=2 を選び、
対応する外部クロック自体がなければ従来の profile=0 を維持します。
別基板では基板に適合した variant / `SystemClock_Config()` と正しい `HSE_VALUE` を用意し、
RCC の読み戻しと周波数の実測を行います。既存の三つの `generic-*-validation` も比較・拒否確認用として残します。

採用方式は **72 MHz の純 CCR1 捕捉**です。元の EXTI コールバックだけが通し番号と `micros()` を記録し、
続いて CCR1 を読みます。TIM1 の更新/CC IRQ は 0 回/秒、DMA 転送は 0 回、他のタイマーは使用しません。
HardwareTimer の setup が有効にする NVIC の UP/CC 入口も、初期化時に明示的に無効化・保留解除します。
TI1 直接入力、立ち上がり、入力分周なし、フィルターなしとし、ISR に待機・整形・UART 出力を追加しません。

`PpsSnapshot.hardware` の `capturedCounter` と `intervalModuloTicks` は **modulo 65536** の証跡です。
公称 1 秒=72,000,000 tick の下位差は **41,472 tick** ですが、これを完全な PPS 間隔と解釈してはいけません。
UIF は上位カウントに使わず、通常の取得では消去もしません。`micros()` から周回数を推測しません。
完全な連続高分解能時刻、完全な PPS 間隔、TIM1 と `micros()` の時刻写像は生成しません。
`intervalModuloValid` は連続した二つの CCR 証跡を表すだけで、正常な 1 PPS や UTC の確認を意味しません。

CC1IF/CC1OF を読み出し前後に確認し、ES096 の CCR 読み出し競合には EXTI pending の再設定も検査します。
捕捉なし・競合・overcapture・通し番号の欠落は間隔証跡を無効化し、再び二つの有効な連続サンプルを必要とします。
設定、RCC、クロック有効状態、所有表、remap、NVIC が変わった場合は `unavailable` をラッチします。
故障検出時に他者のレジスターや NVIC を書き換えず、元に戻すだけでは再開しません。
明示的な `begin()` が必要です。再初期化・GNSS 復旧では古い保留証跡と間隔履歴を破棄します。
`hardwareActive()` は現在の設定も検査します。`snapshot()` は新しい間隔証跡を作りません。

次の捕捉より先に CCR1 を読む必要があります。単独の 1 PPS なら約 1 秒の猶予があり、
その間に 910 µs の周回が複数回起きても下位値は保持されます。異常短パルスでは次のエッジが期限です。
長い割り込み禁止による SysTick の経過欠落は既存の `micros()` の制限であり、本機能では修復しません。

`PpsIntervalTracker`、TIM-TP、NAV-PVT、tentative/confirmed、Bridge 1.0 と Python API は従来の時刻系を維持します。
高分解能証跡で UTC 状態を昇格させません。テキストの `DEBUG_LEVEL>=3` では主ループから
`PPS_HW tick_hz=... unavailable=...` を出力します。利用不能時の `tick_hz` は 0 です。
Binary Bridge にフィールド・フレームを追加しません。

13.89 ns は公称計数分解能です。入力同期・量子化・電気特性・発振器偏差を含む実捕捉誤差、
完全間隔の測定精度、最終 UTC 精度を保証する値ではありません。
根拠、IRQ/DMA/級連の比較、性能見積もり、試験結果は [72 MHz TIM1 審査記録](tim1-pps-review.md)を参照してください。

#### IWDG の動作と限界

同梱の
[STM32duino IWatchdog 1.0.0](https://github.com/stm32duino/Arduino_Core_STM32/blob/2.4.0/libraries/IWatchdog/src/IWatchdog.cpp)
を使用します。LSI の準備、0xCCCC による起動、0x5555 による書込許可、PR/RLR 更新待ち、
0xAAAA によるリロードをライブラリーに任せ、別のレジスタードライバーやタスク管理機構は設けません。
`setup()` 末尾で `begin(24000000)` を一度だけ呼びます。公称 `LSI_VALUE=40000` 以外は拒否します。
設定は固定 24 秒公称、PR=6（/256）、RLR=3749 です。
[DS5319 の LSI 特性](https://www.st.com/resource/en/datasheet/stm32f103c8.pdf)の 30–60 kHz を用いると、
`256 × (3749+1) / fLSI` によりおよそ 16–32 秒となります。正確な 24 秒タイマーではありません。
起動・更新のクロック同期時間もあるため、この式を測定済みのリセット期限とは扱いません。

GNSS 初期化はその時点で完了を待ちません。MON-VER 900 ms、ACK 1,200 ms、baud 切替前後各 100 ms、
signal restart 600 ms、SFRBX 待機 15 秒、再試行、復旧の 30/120 秒、RESET 保持 10 ms・解除後 1 秒は
いずれも複数の loop にまたがる状態機械です。これらの待機中も主処理が進めばリロードします。
UART の設定送信は有限長の UBX、診断は固定長のバッファー・キュー、Bridge pump は空き容量内に制限されます。
OLED は 1 段階ずつ進み、I2C の 3 ms は非同期の総期限です。これらに秒単位の通常ブロッキング待ちはありません。
実機での最大 loop 時間を測ったものではなく、有効化前には最短側の約 16 秒に十分な余裕があることを確認してください。

リロードは **PPS、UART 読出し、GNSS 更新・任意の復旧、診断、Bridge/ログ出力、OLED/LED がすべて戻った
loop の末尾だけ**で行います。ISR からはリロードしません。GNSS が未測位、L1S・通報・PPS が来ない、
OLED 欠落・I2C エラー、ホスト未接続などはリロードを止める理由にしません。
有効時だけ、主ループと baud 再設定での UART 排出をそれぞれ最大 256 byte/回に制限し、
連続受信でも後段処理を飢餓させません。正常に戻る一つの機能だけでリロードを繰り返す処理はありません。
loopback 診断環境では、その診断処理とログ pump の完了後にリロードします。

MCU はリロードが途絶するとリセットされ、起動から GNSS 設定と Bridge セッションを作り直します。
送信途中・未保存のデータは失われ得ます。設定完了前の短い `setup()` 内の停滞は監視対象外です。
ソフトウェア起動後の IWDG は停止できません。既存の option bytes を書き換えず、ソフトウェア起動を前提とします。
ハードウェア起動に設定済みの基板では、`HOSHIGUARD_IWDG=0` でもハードウェアの動作を停止できません。
ライブラリーの LSI 準備待ちは起動前であり、LSI 自体の故障をソフトウェアで救済する保証もありません。
このファームウェアは Stop/Standby を使わず、通常の APB クロックは LSI より十分速いため、ES096 の低速 APB/
Stop 中の PR/RLR 更新制限に該当する省電力構成を追加しないでください。

IWDG はデバッガーのブレーク中も進む場合があります。`DBG_IWDG_STOP` はデバッガー側の設定で動作が変わり、
このファームウェアは自動停止を設定しません。通常のブレークポイント作業では IWDG を無効にしたビルドを使ってください。
復帰原因の診断は追加していません。起動フラグを消去・再解釈せず、電源投入・ソフトウェア・IWDG の原因を
推測して出力しません。EEPROM などへの書き込み、Bridge 1.0 の変更もありません。

判断の基にした公式資料は
[RM0008 の TIM1 入力捕捉・状態レジスター・IWDG](https://www.st.com/resource/en/reference_manual/rm0008-stm32f103xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)、
上記 DS5319、ES096 と、実際にビルドに使用した STM32duino/HAL ソースです。
ソフトウェア回帰・境界試験の手順は[テスト資料](../test/fixtures/README.md#tim1iwdg-のソフトウェア検証と実機境界)を参照してください。
両機能とも実機検証未完了のため、本番での推奨設定は引き続き `0/0` です。

2026-10-31 の 72 MHz 改修に伴う変更前後の Flash/静的 RAM、全環境の構築結果、
ソフトウェア試験と実機試験の区別は [TIM1 審査記録の検証結果](tim1-pps-review.md#ソフトウェア検証と容量)を参照してください。
静的 RAM に実行時 heap/stack 最大値は含みません。

### Explorer の回路照合と未使用回路

確認対象は [2025-06-13 の固定コミット](https://github.com/anyshake/explorer/tree/4daca963e1d449a75016fa40e871b718bce5e7c7)、
その [原理図 PDF の MCU Controller ページ](https://github.com/anyshake/explorer/blob/4daca963e1d449a75016fa40e871b718bce5e7c7/hardware/Explorer.pdf)、
[ファームウェアのピン定義](https://github.com/anyshake/explorer/blob/4daca963e1d449a75016fa40e871b718bce5e7c7/firmware/User/Inc/settings.h)です。
各シートの改訂日付は PCB の日付と一致しない場合があります。この PDF の GNSS は原版 NEO-M8M であり、
MAX-M10S への換装後もリセット・バックアップ電源・通信の接続が同じという利用者のハードウェア情報を前提とします。
AGPL の Explorer ドライバー、RTOS 処理、LED の待機ループは取り込んでいません。

| 回路 | 照合した接続 | 方針 |
|---|---|---|
| SSD1306 | PB6 SCL / PB7 SDA、0x3C | 独立した既存任意機能 |
| 状態 LED D13 | PC13 → R27 1 kΩ → LED → GND | 原版の電流源接続では使用しない |
| AT24CS08 | PB6/PB7、基本 7-bit アドレス 0x50 | 読み書き・走査なし |
| EEPROM WP | ファームウェアは PB5、同コミット原理図は PA0、PB5 は未接続 | 資料不一致。どちらの GPIO も操作せず、実基板の導通確認が必要 |
| SW4 OPTIONS | 第 1 位 PB12、第 2 位 PB11、第 3 位 PB10 | 第 1 位だけ OLED 抑止に利用 |
| SW5 BAUDRATE | PB8/PB9 | 読まない。Bridge の 115200 baud を維持 |
| SW3 SAMPLERATE | PB3/PB4 | 読まない。JTAG/SWO との競合を増やさない |
| GNSS RESET_N | PA15 | 明示的な復旧環境のみ |
| GNSS バックアップ電源 | BT1 DMS3R3、D7 SS34、V_BCKP 系統 | 既存回路を自然利用。測定・充電制御を追加しない |

EEPROM の WP は [AT24CS08 データシート](https://ww1.microchip.com/downloads/en/DeviceDoc/20006350A.pdf)では
High で書き込み保護です。現在は保持すべき可変設定がなく、OLED 設定はコンパイル指定と起動スイッチだけで足ります。
永続設定、識別番号の読み出し、受信カウンター保存、I2C スキャンは追加しません。
そのため EEPROM 欠落・内容破損は起動に影響せず、EEPROM 用の I2C 仲裁や書き込み・耐電断処理も不要です。
EEPROM が物理的に SDA/SCL を固着させた場合は OLED 側が期限超過で停止し、受信を継続します。
ADC、検波器、加速度計・ジャイロ、PGA/PGV/PGD・震度処理は対象外です。

### PC13 状態 LED

[STM32F103x8/xB データシート DS5319、ピン表の注記](https://www.st.com/resource/en/datasheet/stm32f103c8.pdf)は
PC13–PC15 を電流源として LED 駆動に使うことを禁止しています。原版 Explorer の D13 はこの接続なので、
高電平の push-pull 駆動は実装しません。
`generic-status-led` は **3.3 V → 電流制限抵抗 → LED → PC13** の吸電流接続専用です。
3 mA 未満、負荷 30 pF 以下、2 MHz 以下の条件を守り、ほかの PC14/PC15 出力との制約も確認してください。
ファームウェアは open-drain・低速設定を使い、OFF は解放、ON は Low とします。
Explorer で利用する場合は先に電気的に適切な接続へ改修する必要があり、ソフトウェアだけでは対応できません。

| 表示 | パターン |
|---|---|
| 初期化中 | 500 ms 周期、250 ms 点灯 |
| 動作中 | 2 秒周期、50 ms の短い点灯 1 回 |
| 注意 | 2 秒周期、100 ms 点灯 2 回。設定の制限または有効 UBX が 10 秒以上途絶 |
| 初期化失敗 | 1 秒周期、100 ms 点灯 3 回 |

処理は主ループ末尾の時刻差分と GPIO 書き込みだけで、待機・割り込み・タイマーを追加しません。
L1S の未観測・災害通報の空白・SFRBX 検証待ち期限だけでは故障表示にしません。
有効 UBX が続けば通常の待ち受けとして扱い、LED の状態は災害の有無を表しません。

### 起動時の OLED 抑止スイッチ

`HOSHIGUARD_OLED_SWITCH=1` かつ OLED 有効のときだけ、起動時に SW4 OPTIONS 第 1 位（PB12）を一度読みます。
原図では 10 kΩ プルダウン、ON で 3.3 V に接続されます。内部プルダウンも有効にし、OFF/未接続は従来の表示、
ON は起動中ずっと OLED を抑止します。ON 読み取りは保持し、接点の安定待ち・実行中の切り替え・EEPROM 保存は行いません。
誤読時の影響は表示の有無に限定されます。残りのスイッチはどの位置でも無視し、UART、GNSS、PPS、Bridge に影響しません。

### MAX-M10S のバックアップ電源と限定的なハードリセット

[MAX-M10S Data Sheet UBX-20035208 R08](https://content.u-blox.com/sites/default/files/MAX-M10S_DataSheet_UBX-20035208.pdf)、
[Integration Manual UBX-20053088 R05 §3.2.3.1 / §3.6.3.1](https://content.u-blox.com/sites/default/files/MAX-M10S_IntegrationManual_UBX-20053088.pdf)
を確認しました。V_BCKP の動作範囲は 1.65–3.6 V で、適切な電圧が維持されれば BBR、RTC、軌道情報を保持し、
次の起動を補助できます。既存初期化は起動時のコールドリセットを行わず、設定を読み戻して必要な変更だけを行うため、
この回路の恩恵を受けられます。既定の `CONFIGURE_GNSS_BBR=0` は設定の RAM 書き込みを維持します。
電池電圧・保持成功・ホットスタート成功を測定する入力はないので、OLED や Bridge にそれらの状態を追加しません。

RESET_N は V_IO 基準の入力、内蔵プルアップ付き、Low **1 ms 以上**が要件です。
Low 入力上限は 0.63 V、入力動作範囲は 0–V_IO。GND への容量追加は避けます。
使用する出力は PA15 の open-drain とし、High を能動出力せず通常は解放します。
MAX-M10S の V_IO、PA15 との導通、共通 GND、電源投入順を実基板で確認してください。
主電源が切れた GNSS の PIO は駆動してはいけません。MCU と GNSS が別電源の場合は UART/PPS も含めて絶縁・分離を検討します。

**RESET_N は BBR・RTC・軌道情報を消し、コールドスタートを起こすため、通常運用の電波回復には使用しません。**
`HOSHIGUARD_GNSS_RESET=1` の復旧は次の条件に限定します。

1. この起動で checksum-valid UBX または PPS が一度でも観測されれば、その後途絶しても自動復旧を永久に抑止する。
2. 既存の全 baud 探索が `Failed` になり、起動から 30 秒以上経過したとき、まずソフトウェア初期化を 1 回だけやり直す。
3. その再試行も `Failed`、そこから 120 秒以上経過、さらに UART 受信 byte が連続 30 秒以上ない場合だけハードリセットする。
   NMEA や破損 byte が継続している場合もハードリセットしない。SFRBX/L1S、測位品質、災害通報の有無は条件に含めない。
4. Low を 10 ms 以上保持し、解除から 1 秒の起動待ちを経て UART 再オープン・baud 探索・全設定検証へ戻す。
   各待ち時間は主ループの状態遷移であり、`delay()` は使わない。1 秒は保守的な実装上の待機値で、実機の準備完了を保証しない。
5. ハードリセットは MCU 起動あたり最大 1 回。成功・失敗や時間経過で再許可しない。継続する電源断・MCU 再起動をまたぐ制限はない。

保持・起動待ち中も USART2 は読み捨てて UART キューを排出し、復旧前の未完 UBX、PPS 間隔・保留エッジ、
UTC/PPS/TIM-TP 関連付けを捨てます。設定の読み戻し証拠も作り直し、古い時刻ロックを引き継ぎません。
PPS の ISR 通し番号、累積 UBX カウンター、送信済み・送信待ちの完全な Bridge フレームは維持します。
復旧に実際の入力が重なれば取りこぼし得ます。起動時の無通信に限定して、そのリスクと BBR 消去を抑えます。
動作中に一度成立した受信をハードリセットで壊す設計にはしません。

無効時は PA15 に対する初期化・書き込みも行いません。有効時だけ
[RM0008 の SWJ_CFG=010](https://www.st.com/resource/en/reference_manual/cd00171190-stm32f101-103-105-107-stm32f100-series-armbased-32bit-mcus-stmicroelectronics.pdf)
で JTAG を解放し、SWD（PA13/PA14）を維持します。PB3/PB4 の GPIO は操作しません。
完全 JTAG デバッグとの併用はできません。SWD による実機書き込み・デバッグの継続確認は別途必要です。

### RS-232 / RS-485 と TTL

Explorer の ADM3202、MAX3485、TTL ヘッダーは **同じ USART1 PA9/PA10** の物理変換です。
ファームウェアは引き続き 115200 baud、8N1、フロー制御なしの Bridge Binary 1.0 を出力します。
USART2 は GNSS 専用であり、スイッチを変えても両者の速度は変わりません。

| 接続 | Explorer 側 | ホスト側 |
|---|---|---|
| 3.3 V TTL J6 | pin 1 TX、pin 2 RX、pin 3 GND | USB-UART RX、TX（必要時）、GND |
| RS-232 J7 メス | pin 2 TX、pin 3 RX、pin 5 GND | RS-232 アダプター RX、TX、信号 GND |
| RS-232 J8 オス | pin 3 TX、pin 2 RX、pin 5 GND | RS-232 アダプター RX、TX、信号 GND |
| RS-485 J4 | pin 1 A、pin 2 B | USB-RS485 A/B（メーカーの A/B 表記差に注意） |

RS-232 の電圧を TTL/GNSS 端子へ接続しないでください。DE/RE は MCU の独立 GPIO ではなく、
74HC14 と TX 信号による自動制御回路です。追加の方向切替ドライバーは不要です。
RS-485 は DI=GND、TX の Low 区間で送信を有効化し、High 区間はバイアス抵抗による復帰に依存します。
基板側に 120 Ω 終端があり、対向終端・共通モード範囲・配線長・baud に応じた波形確認が必要です。
HoshiGuard は単一送信機のポイント間出力として扱い、多数局のアドレス制御や衝突回避は追加しません。
複数の RX 入力元を同時に駆動せず、通常は一つの物理接続だけを使用してください。
ホストは既存のシリアルポート指定で動作し、プロトコル変更や追加設定は不要です。

### 追加機能のソフトウェア検証と資源使用量

2026-10-31、変更前コミット `6137973` と同じ STM32 platform 15.6.0、STM32duino 2.4.0、GCC 9.2.1、
`-Os -flto`、`SOURCE_DATE_EPOCH=1793404800` で比較しました。Flash は `.bin` 全体、RAM は `.data + .bss` です。

| 構成 | Flash（byte） | 静的 RAM（byte） |
|---|---:|---:|
| 変更前・変更後の従来既定（OLED 有効） | 48,068 | 11,036 |
| `generic-minimal` / Explorer の OLED 無効・最小構成 | 43,476 | 10,488 |
| `generic-status-led` | 43,984 | 10,500 |
| `explorer-e-c111g` | 48,164 | 11,036 |
| `explorer-e-c111g-recovery` | 49,016 | 11,056 |
| LED・OLED・表示スイッチ・復旧をすべて有効にした一時検証構成 | 49,420 | 11,072 |
| 同構成から OLED だけを無効 | 45,112 | 10,524 |

表示スイッチは従来既定比 +96 byte Flash、静的 RAM 増分 0、復旧追加は Explorer 標準比 +852 / +20 byte です。
吸電流 LED は最小構成比 +508 / +12 byte です。LTO により組み合わせの増分は単純加算になりません。
従来既定は変更前と `.bin` の SHA-256 が一致し、最小構成も変更前の OLED 無効版と一致しました。
最小構成の ELF には OLED・追加 GPIO・補助状態機械のシンボルがありません。
PlatformIO の Flash 表示値は `.bin` より 288 byte 小さく、静的 RAM は実行時スタックの最大使用量ではありません。

基線は native 137 件、Python 142 件、tools 38 件が成功しました。変更後は native 145 件、
同じ 145 件の ASan/UBSan、Python 142 件、tools 38 件、実ファームウェア入口の ASan/UBSan 模擬 9 シナリオが成功しています。
既存環境を含む STM32 16 環境と一時組み合わせ 2 環境をビルドしました。
OLED の欠落・NACK・BUSY/STOP 固着・通信期限・異常入力、UBX/L1S/CRC、PPS/TIM-TP、Bridge 1.0 の回帰を含みます。
再現コマンドは[テスト資料](../test/fixtures/README.md#検証コマンド)を参照してください。

これらはソフトウェア検証です。実機の PA15 導通・V_IO・Low 幅と再起動、バックアップ保持、SWD、
スイッチの位置表示、I2C 波形・固着、RS-232/RS-485、連続 UART 負荷時の overrun・PPS 遅延・主ループ最大時間は未測定です。
EEPROM の資料間 WP 不一致と PC13 LED の電流源接続はハードウェアで確認が必要です。
書き込み、電断、GNSS 故障復旧を含む実機試験が済むまでは、ビルド成功を実機確認として扱わないでください。

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
