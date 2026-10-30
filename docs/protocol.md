# Bridge・放送仕様と対応範囲

## 参照仕様

実装が参照する版と元資料の SHA-256 は [spec-inventory.json](spec-inventory.json) に記録します。
公式サイトの現行版と実装対象版は必ずしも一致しません。PDF は同梱せず、版・ハッシュを照合して参照してください。

| 資料 | 実装での用途 |
|---|---|
| [IS-QZSS-L1S-009](https://qzss.go.jp/en/technical/download/pdf/ps-is-qzss/is-qzss-l1s-009.pdf) | L1S の伝送・CRC・MT 振り分け |
| [IS-QZSS-DCR-017](https://qzss.go.jp/en/technical/download/pdf/ps-is-qzss/is-qzss-dcr-017.pdf) | MT43。016 のビット配置を維持し、時刻・コード定義を追加 |
| [IS-QZSS-DCR-016](https://qzss.go.jp/en/technical/download/pdf/ps-is-qzss/is-qzss-dcr-016.pdf) | 既存 DCR デコーダーと試験シナリオの基準 |
| [IS-QZSS-DCX-004](https://qzss.go.jp/en/technical/download/pdf/ps-is-qzss/is-qzss-dcx-004.pdf) | MT44 の構造・拡張 |
| [EWSS CAMF 1.2](https://www.gsc-europa.eu/sites/default/files/sites/all/files/EWSS-CAMF_in_force.pdf) | A1–A18、災害・指示ライブラリ |

[QZSS 公式仕様一覧](https://qzss.go.jp/en/technical/ps-is-qzss/ps-is-qzss.html)、
[アプリケーションノート](https://qzss.go.jp/technical/dod/dc-report/users-manual.html)、
[DCR 試験配信](https://qzss.go.jp/technical/dod/dc-report/test-data-distribution.html)、
[DCX 試験配信](https://qzss.go.jp/technical/dod/dc-report/dcx-test-data-distribution.html)も参照できます。
古い DCR-013 ベースの説明資料でビット配置を上書きしません。月別の試験計画は期待シナリオの資料であり、
ビット仕様や架空の放送バイナリを作る根拠には使いません。保存済みの 2026 年 7 月 manifest は[回帰テスト資料](../test/fixtures/README.md)です。

## Binary Bridge 1.0

伝送形式は `COBS(logical frame) + 00`、複数バイト整数はリトルエンディアンです。
論理フレームは固定ヘッダー、payload、ヘッダーと payload を対象とした CRC32 の順に並びます。
CRC は CRC-32/ISO-HDLC: polynomial `0x04C11DB7`（反転実装 `0xEDB88320`）、
init/xorout `FFFFFFFF`、refin/refout true、`123456789` の検査値 `CBF43926` です。CRC 自体もリトルエンディアンです。

| オフセット | バイト数 | フィールド |
|---:|---:|---|
| 0 | 2 | `HG` |
| 2 | 1 | major = 1 |
| 3 | 1 | minor = 0 |
| 4 | 1 | frame type |
| 5 | 1 | reserved |
| 6 | 2 | header length = 24 |
| 8 | 2 | payload length |
| 10 | 2 | header flags |
| 12 | 4 | sequence |
| 16 | 8 | 拡張 STM32 timestamp_us |

major 不一致は拒否します。新しい minor は、固定ヘッダーと payload 長を解釈できる範囲で受理します。
sequence は modulo 2^32 です。小さい sequence に戻った HELLO は再起動の epoch として扱い、通常の欠落と区別します。

| frame type | 値 | payload の長さ・内容 |
|---|---:|---|
| HELLO | 1 | 64 byte。プロトコル・ファームウェア・機能・baud・容量・ビルド情報。60 秒周期 |
| L1S_FRAME | 2 | 20 byte のメタデータと完全な UBX-RXM-SFRBX |
| STATUS | 3 | 88 byte。稼働時間、baud、GNSS/PPS/時刻状態、受信・キュー・欠落カウンター |
| TIME_STATUS | 4 | 32 byte。PPS と TIM-TP の時刻根拠 |
| ERROR | 5 | 12 byte。code U2、reserved U2、evidence0/evidence1 U4 |
| HEARTBEAT | 6 | 4 byte の uptime_ms |

L1S_FRAME のメタデータ順序は PPS sequence U4、svId、gnssId、sigId、numWords、PAB、MT、flags U2、
UBX length U2、time quality、PPS state、firmware major/minor/patch、reserved です。
その後に同期・長さ・全 dwrd・チェックサムを含む UBX が続きます。Python は検証を繰り返し、
MCU の申告 flags と実検証が一致しないフレームを意味解析に流しません。

TIME_STATUS は PPS sequence、PPS/quality/reason、TIM-TP flags、符号付き qErr（公式単位 ps）、
qErr 有効フラグ、refInfo、GPS week、raw towMS/towSubMS、対応 PPS sequence、matched/ambiguous/time-grid-known bits の順です。
利用不能な qErr は Python で `null` とし、数値プレースホルダーを誤差ゼロと解釈しません。
各 payload の完全な対応は [bridge_frames.py](../python/hoshiguard/bridge_frames.py) と
[bridge_transport.cpp](../src/bridge_transport.cpp)で確認できます。

固定 7 スロットの送信キューは L1S 用に 4 スロットを予約します。送信中の COBS レコードは置換せず、
UART の部分書き込みでもフレーム境界を守ります。Critical/L1S は低優先の STATUS/TIME を置換できます。
低優先フレームは予約枠を消費できず、enqueue は UART を待ちません。

STATUS は Bridge generated/queued/sent/dropped、Critical dropped、L1S generated/sent/dropped を区別します。
セッションの最初と最後の STATUS 差分は、その sequence 窓の内側の観測フレームと比較します。
STATUS の payload 作成時点では当該フレーム自体が送信済みとは限らず、境界で sent に 1 件の差が出る場合があります。
長時間検証では `generated-sent` の未送信分を次の窓へ引き継ぎ、厳密な snapshot 比較とキュー補正後の比較を別々に報告します。
意味識別や出力フィルターは PC 側の処理であり、Bridge 1.0 の wire format を変更しません。

## HGRAW 証跡形式

[raw_archive.py](../python/hoshiguard/output/raw_archive.py) の HGRAW 1.0 は次のリトルエンディアン形式です。

| 部分 | 構造 |
|---|---|
| ファイルヘッダー 16 byte | `HGRAW1\0\0`（8 byte）、major U2=1、minor U2=0、reserved U4=0 |
| レコード先頭 8 byte | `HGRF`、以後の長さ U4（`16 + wire length`） |
| レコード本体 | PC receive time U8（Unix ns）、wire length U4、末尾 `00` を含む完全な Bridge wire frame |
| レコード末尾 4 byte | レコード本体の CRC32 U4 |

レコード CRC32 は受信時刻・wire length・wire を対象とする CRC-32/ISO-HDLC です。Bridge 内の CRC とは独立して検証します。
レコード先頭の長さは本体と末尾 CRC を含み、読み取り側は 16–4096 byte の範囲を許可します。破損時は次の `HGRF` を探して再同期します。
未知のファイルヘッダーへ追記せず、元の証跡を上書きしません。PC 受信時刻と MCU timestamp は別の時間軸です。

## L1S と MT43（DCR）

ビット 0 は PAB の MSB、以下の区間は半開区間 `[開始, 終了)` です。
L1S は有効 250 bit、CRC は `[226,250)`、計算対象は `[0,226)`、末尾 6 bit は padding です。
UBX の各 U4 はリトルエンディアンですが、数値の bit 31 が先に送信されるため、各 word 内のバイト順だけを反転します。
MAX-M10S SPG 5.10 の実受信は 9 word、既存試験ベクトルは 8 word で、両者以外の個数を無条件に受理しません。
最初の 8 word が L1S 本体で、9 番目は診断用に保持します。
MT43 は DCR、44 は DCX、47–51 は SLAS、63 は NULL、40/41 は予約です。
SLAS/Null/予約 MT は災害の発表として扱いません。

| 共通フィールド | ビット区間 | 幅 | 出力 |
|---|---|---:|---|
| 通報区分 | [14,17) | 3 | 原コード、名称、status |
| 災害カテゴリ | [17,21) | 4 | カテゴリ別 payload と原コード |
| 月・日・時・分 | [21,41) | 20 | 原値と推定年 |
| 情報種別 | [41,43) | 2 | 発表・訂正・取消 |
| DCR version | [214,220) | 6 | 原値 |
| CRC | [226,250) | 24 | 独立検証 |

カテゴリ別デコードは MIT ライセンスの azarashi に基づき、[dcr/decoder.py](../python/hoshiguard/dcr/decoder.py)でラップします。
各 offset・尺度・単位・特殊値は DCR-016/017 の 4.1.2 と、同梱するカテゴリ別 decoder/definition に対応します。

| コード | 内容 | payload 型・主要テーブル |
|---:|---|---|
| 1 | 緊急地震速報 | `EarthquakeEarlyWarningPayload`、震度・震央・予報区 |
| 2 | 震源 | `HypocenterPayload`、震央・規模・深さの特殊値 |
| 3 | 震度 | `SeismicIntensityPayload`、都道府県・震度 |
| 4 | 南海トラフ地震 | `NankaiTroughPayload`、情報番号・18 byte ページ |
| 5 | 津波 | `TsunamiPayload`、沿岸地域・高さ・警報 |
| 6 | 北西太平洋津波 | `NorthwestPacificTsunamiPayload`、発生可能性・高さ |
| 8 | 火山 | `VolcanoPayload`、火山・市町村・警報 |
| 9 | 降灰 | `AshFallPayload`、降灰警報・市町村 |
| 10 | 気象 | `WeatherPayload`、警報状態・予報区 |
| 11 | 洪水 | `FloodPayload`、警戒レベル・予報区 |
| 12 | 台風 | `TyphoonPayload`、規模・強さ・基準時刻 |
| 14 | 海上 | `MarinePayload`、海上警報・予報区 |

0/7/13/15 は予約値です。南海トラフ Pn/Pm の再構成には容量・時間上限があります。
017 はビット配置を維持し、津波の時刻・高さの「該当情報なし」と洪水のコード・名称を追加します。
津波の `(day,hour,minute)=(0,30,62)` と既存の `(0,31,63)` を区別し、不正な組み合わせは拒否します。
高さコード 13 と 14 も区別して原値を残します。推定震源の De/Ma=10 は公開層で実測値として扱いません。
これらの追加定義は合成ベクトルによる検証であり、新たな実放送検証とは区別します。

## MT44（DCX / CAMF 1.2）

A1–A18 の位置は [camf.py](../python/hoshiguard/dcx/camf.py) に集約し、
拡張は [extended.py](../python/hoshiguard/dcx/extended.py) に実装します。
意味テーブルと計算は出所を明示した vendor コードで解釈し、Human と NDJSON は同じ UnifiedAlert を参照します。

| フィールド | ビット区間 | 内容 |
|---|---|---|
| SD mask type / mask | [14,15) / [15,24) | 伝送指定の原値 |
| A1 | [24,26) | Message Type |
| A2 | [26,35) | 国・地域 |
| A3 | [35,40) | A2 に依存する提供者 |
| A4 | [40,47) | 災害コード 1–113 |
| A5 | [47,49) | 重大度 0–3 |
| A6 | [49,50) | 発生週。原値と推定時刻を区別 |
| A7 | [50,64) | 週内分の原値 |
| A8 | [64,66) | 継続時間区分 |
| A9 | [66,67) | 国際・国別ライブラリ |
| A10 | [67,70) | ライブラリ版 |
| A11 | [70,80) | 指示文コード |
| A12 | [80,96) | 主楕円の緯度 |
| A13 | [96,113) | 主楕円の経度 |
| A14 | [113,118) | 主楕円の長半径 |
| A15 | [118,123) | 主楕円の短半径 |
| A16 | [123,129) | 主楕円の方位角 |
| A17 | [129,131) | Specific Settings の種類 B1/B2/B3/B4 |
| A18 | [131,146) | A17/A4 に応じた C/D フィールド |
| EX1 | [146,162) | 対象地域コード |
| EX2 | [162,163) | 避難方向 |
| EX3 | [163,180) | 追加楕円の緯度 |
| EX4 | [180,197) | 追加楕円の経度 |
| EX5 | [197,202) | 追加楕円の長半径 |
| EX6 | [202,207) | 追加楕円の短半径 |
| EX7 | [207,214) | 追加楕円の方位角 |
| EX8 | [146,147) | J-Alert 地域リスト種別 |
| EX9 | [147,211) | 都道府県マスクまたは 4 地域コード |
| EX10 | [211,214) | 予約値を保持 |
| EX11 | [146,214) | 国別拡張 68 bit の原値 |
| Vn | [214,220) | DCX version |

CAMF と Extended Message は単一 MT44 に含まれ、DCX-004 はフレームをまたぐ再構成を規定しません。
EX9 の都道府県モードは下位 47 bit のみを対象とし、上位 17 bit は予約値です（図 4.2-7、表 4.2-25）。
日本の non-Null は Vn=1 に対応します。日本以外の Vn は国別仕様に依存します。
EX11 は `raw_only` とし、国別仕様がない状態で意味を推測しません。
市町村の同一性には原コードを使い、名称は `name_source=snapshot, authoritative=false` です。
現行の権威ある JIS X 0402 データセットとしての独立検証は済んでいません。

## 対応範囲と証拠の強さ

機械可読の [support_matrix.json](support_matrix.json) は、実装と証拠を分けて管理します。
`unsupported` / `raw_only` / `implemented` / `synthetic_verified` / `official_scenario_defined` /
`official_scenario_verified` / `real_broadcast_verified` を混同しないでください。
DCR の有効 12 カテゴリ、DCX A1–A18・A4 1–113・EX1–EX9 は実装と合成検証があります。
EX10 は予約値を保持し、EX11 と市町村名には上記の制約があります。
2026 年 7 月 DCR シナリオは定義済みですが、対応する実受信の照合証跡はありません。
未受信はデコード失敗とは限らず、優先する正式通報により試験配信が中断される場合があります。

ファームウェア 0.2.5 には約 87,249 秒の実機 Bridge 1.0 証跡があります。
これは現在のバイナリの 24 時間実機試験完了を保証しません。同セッションで観測した意味は
DCR14、DCX Null、69 フレームの non-Null `l_alert_ex1` に限られ、未観測カテゴリへ証拠を拡張しません。
元の完全アーカイブと抜粋 fixture は異なります。ハッシュ・選択基準・未検証範囲は[テスト資料](../test/fixtures/README.md)を参照してください。
