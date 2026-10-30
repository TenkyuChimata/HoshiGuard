# HTTP / WebSocket API とデータ形式

同一プロセス内の読み取り専用 API です。受信側が Bridge・UBX・L1S を独立検証し、
単一のデコード処理から生成した状態を参照します。HTTP はファイルを走査せず、WS は再デコードしません。
起動と公開設定は[運用ガイド](deployment.md)を参照してください。

## HTTP

応答は `Content-Type: application/json`、`Cache-Control: no-store` です。ETag は実装していません。

| GET パス | 応答 |
|---|---|
| `/api/v1/alerts/latest` | 最新の該当 PublicAlert。該当なしは `data: null` |
| `/api/v1/alerts` | 新しい順の PublicAlert 配列。該当なしは `data: []` |
| `/api/v1/alerts/{id}` | キャッシュ内の通報。期限切れ・削除済みは 404、`data: null` |
| `/api/v1/status` | 健全性を表すオブジェクトを直接返す |

通報応答の形式例です。`data` と `status` を省略した説明用の例であり、完全な応答サンプルではありません。

```json
{"schema":"hoshiguard.api.v1","process_id":"00000000-0000-4000-8000-000000000000","sequence":17,"data":null,"next_offset":null,"status":{}}
```

`/alerts` と `/alerts/latest` のクエリ:

| パラメーター | 値・既定値 |
|---|---|
| `service` | `DCR` / `DCX`。省略時は両方 |
| `category` | `category.token` の完全一致。例: `MARINE` / `MISSILE_ATTACK` |
| `status` | `actual`（既定）/ `test` / `unknown` / `all` |
| `limit` | 1–100、既定 100。latest は常に 1 |
| `offset` | 0–history-limit、既定 0 |
| `view` | `history`（既定）/ `states`。後者は保守的に関連付け可能な単位の最新状態 |

未知・重複パラメーターは 400、許可しないメソッドは 405、該当なしは 404、レート超過は 429 です。
Origin 拒否は 403、1024 文字超のクエリや本文・Transfer-Encoding 付き要求は 413 です。
WS の接続上限ではハンドシェイクを 503 で拒否します。アプリケーションエラーは安定した識別子のみを返し、例外詳細はローカルログに記録します。

一覧内の通報 JSON は最大 1 MiB です。`next_offset` または実際の返却件数を使って次を取得し、
空配列まで繰り返してください。末尾確認に追加の空リクエストが必要な場合があります。
履歴は変動するメモリ窓です。ページング中に `sequence` が変化した場合は再取得してください。
単一応答の `data`・`sequence`・`status` は同じロック内で取得しますが、複数要求は同一トランザクションではありません。

ID 指定には `status` フィルターがなく、訓練メッセージの ID を持つクライアントはその内容を取得できます。
正式情報の既定一覧と WS には訓練情報を含めません。認証・管理・書き込み機能はありません。

## PublicAlert

[JSON Schema](public-alert.v1.schema.json) の識別子は `hoshiguard.public-alert.v1` です。
v1 の互換拡張ではフィールドを追加できるため、クライアントは未知のフィールドを無視してください。
既存フィールドの型・意味を破壊的に変更する場合はメジャーバージョンを更新します。

| フィールド | 意味 |
|---|---|
| `schema` | 公開モデルのバージョン |
| `id` | `sf2-` と完全な semantic fingerprint v2。通報内容を識別 |
| `source` | QZSS の放送元。JMA または CAMF country/provider の原コードと既知の名称 |
| `service` | `DCR` / `DCX` |
| `category` | 原コード、token、known/reserved、利用可能な名称 |
| `status` | `actual` / `test` / `unknown`。DCR Rc=7、DCX A1=0、試験災害コード 13/113 は `test` |
| `action` | `issue` / `update` / `cancel` / `clear` / `unknown` |
| `identity` | fingerprint の版、`event_id: null`、信頼度に制限のある関連付けの手掛かり |
| `times` | 発表・受信・発生・失効時刻、時間の根拠、必要な原コード |
| `areas` | 通報対象地域のコード・名称・kind。受信機の座標ではない |
| `severity` | 仕様に含まれるレベル・警報の原コードと説明。未提供時は `null` |
| `payload` | 災害別の `type` と各フィールド |
| `validity` | 伝送検証、意味解析、完全性、カテゴリの既知性、未知フィールド |
| `is_real_disaster` | 検証を通過した正式かつ既知の災害通報。訓練・試験カテゴリは `false` |
| `provenance` | 仕様、`live` / `replay`、データ形式。JMA XML 原文ではない |

DCR の `payload.type` は earthquake_early_warning、hypocenter、seismic_intensity、
nankai_trough_earthquake、tsunami、northwest_pacific_tsunami、volcano、ash_fall、weather、
flood、typhoon、marine です。`payload.fields` はカテゴリ別の値と `_raw` を保持します。
It=1 の訂正は公開 `action=update` とし、原値と Correction ラベルも残します。
EEW の `assumptive=true` では固定マーカー De/Ma=10 を実測深度・マグニチュードと解釈せず、
公開値を `null` として原値と `measurement_basis` を残します。欠けた震源情報や震度をゼロで補いません。

DCX の `payload.type` は CAMF hazard token に基づきます。`camf` に A1–A18、`guidance` に
解読した指示文、`extended` に拡張の種類と値を保持します。避難指示は provider・A11・EX に依存するため、
hazard や A1 だけで判断しないでください。siren/test カテゴリは A1=Alert でも公開範囲を `test` とし、原 A1 と内部識別子は保持します。
J-Alert の避難呼び掛けの撤回も A1=Alert の場合があり、指示文から災害全体の `clear` を推定しません。
EX9 は 64 bit 整数に加えて固定 16 桁の `EX9_target_area_code_list_hex` を提供し、JavaScript 等での丸めを避けます。
EX11 は原値のみを保持して不完全と表示し、未知のライブラリ版では A11 の指示文を解釈しません。

未知・予約値は原値を保持します。未対応の業務ヘッダーは有効な公開通報として配信せず、Bridge 証跡には保存できます。
既知の主要部分のみを解読した `complete: false` の通報は配信できるため、必ず `validity` を確認してください。
`is_real_disaster` は警報の継続中を意味せず、暗号学的な真正性の証明でもありません。

## 時刻と識別子

DCR の月・日・時・分は UTC で、年を含みません。年は PC の受信日時から推定し、
HGRAW 再生では元の PC 時刻を使います。派生日時には年が推定であることを明示します。
ISO 8601 は `+00:00`、JST 表示は UTC+9 です。放送の原値を JST と解釈して再度 9 時間引かないでください。
DCX A6/A7 の週・週内分は原値を残し、公開層では絶対 `onset` / `expires` を推定しません。
受信時刻から発表時刻を捏造したり、TTL 切れを `clear` に置き換えたりしません。

`id` は災害イベント ID ではありません。年の欠落や相対週の循環により、別年の同内容が同じ ID になる場合があります。
`event_key` は `correlation_hint` として公開し、現在の信頼度は `limited` です。強制的な災害統合には使いません。
同一内容の衛星間反復を除外し、訂正・取消・解除はそれぞれの内容 ID で履歴に残します。
`states` にも同一災害の複数通報が残り得ます。関連性を証明できない場合の保守的な表示です。

## WebSocket

`ws://127.0.0.1:8000/api/v1/ws` に接続します。クエリは `status=actual|test|unknown|all` のみで、既定は `actual` です。
以下は伝送形式の説明用の例です。`data` は省略しています。

```json
{"type":"alert","process_id":"00000000-0000-4000-8000-000000000000","sequence":18,"data":{}}
```

| `type` | `data` |
|---|---|
| `snapshot` | `alerts`（最新状態、最大 100 件・1 MiB）、`truncated`、同時点の `status` |
| `alert` | 新しく確定した PublicAlert。災害種別は `data.category` |
| `status` | 接続・有効 L1S の状態やエラー数の変化に伴う健全性情報 |

購読登録とスナップショットは同じロック内で行い、最初に snapshot、その後に変更を送ります。
接続上限は 64、各送信キューは 32 件です。全購読でエンコード済み文字列を共有し、受信スレッドはネットワークを待ちません。
キュー超過は 1013、異常は 1011、サーバー終了は 1001、クライアントによる業務 TEXT/BINARY 送信は 1008 で閉じます。
送信が 5 秒を超えると接続を中止し、close の drain と応答待ちの合計は最大 2 秒です。
aiohttp が ping/pong と 30 秒の heartbeat を処理します。アプリケーション用の心拍 JSON は不要です。

再接続はバックオフを設け、新 snapshot でローカル表示を置き換えてください。
`truncated` の場合は WS 更新を処理しながら `/alerts?view=states` をページングし、`sequence` を確認します。
`process_id` の変更は再起動を示します。`sequence` は status を含むプロセス内の確定順序で、業務通知には欠番が生じます。
災害 ID や再起動をまたぐ再送カーソルには使えません。完全なイベントログや旧 sequence からの再送は提供しません。
復元範囲はキャッシュ容量・TTL・再起動に依存します。完全な伝送証跡が必要な場合は HGRAW を保存してください。

## 健全性と資源上限

`receiver` は starting/waiting_hello/connected/disconnected/stopped/failed/replay/replay_complete、
`link` は接続状態、`signal` は receiving/waiting_l1s/no_l1s/unknown/historical を表します。
15 秒間 Bridge フレームがないと `stalled`、Bridge 通信中に 30 秒間有効な L1S がないと `no_l1s` です。
SLAS/Null も有効受信時刻を更新するため、災害通報が来ないだけでは異常になりません。
`no_l1s` は衛星が全く見えないことの証明ではありません。

切断・再生・受信停止時はキャッシュを残して `data_mode=historical` とします。
`live` は最近有効な L1S を受信したことを示すだけで、キャッシュ内の個々の通報が新しいとは限りません。
通報時刻は `times` で確認してください。エラー数はプロセス内の累積値です。
`decode_errors`、`verification_errors`、`bridge_errors`、`sequence_gaps`、`upstream_errors`、
`upstream_drops`、`upstream_l1s_drops`、`archive_errors` を区別し、`last_decode_error` は種類のみを公開します。
抜粋した HGRAW には選択に伴う sequence の飛びがあり、元の現場での欠落を意味しません。

履歴は既定 512 件・24 時間、意味的重複排除は 4096 件・7 日です。履歴 TTL は公開確定時から計算し、
反復放送で元の発表時刻や保持期限を延長しません。TTL はホストの monotonic clock を使い、MCU 再起動や壁時計の巻き戻りに依存しません。
履歴の失効と受信の健全性は別々に判定します。

同一オリジンのブラウザーは既定で使用できます。クロスオリジンは `--api-origin` で明示し、`*` は指定できません。
Origin のない通常クライアントも公開 API を読めます。既定は全体で 100 リクエスト/秒、burst 200 です。
IP ごとの無制限な管理表は持たず、TLS・アドレス別制限・header timeout はリバースプロキシで設定します。
アプリケーション自体は TLS、ログイン、管理 API、原証跡ダウンロードを実装しません。

## 実受信の例と内部診断形式

[Marine](public-alert-real-marine.json) と [DCX](public-alert-real-dcx.json) は、同梱 HGRAW を現在の実装で
直接デコードした実受信例です。`input=replay` は今回の読み取り方法で、`times.received` は元の PC 時刻です。
出所・再生成手順は[テスト資料](../test/fixtures/README.md#公開-json-例)を参照してください。

内部の不変モデル `hoshiguard.alert.v1` は Human と診断 NDJSON で共有します。
`frame_fingerprint` は伝送証跡、semantic fingerprint v2 は内容を識別し、後者は受信時刻・Bridge/PPS・衛星・PAB・
UBX/L1S raw・チェックサム・表示ラベルを除外します。status/action/category、仕様上の時刻原値、provider/library、
対象地域、順序付き payload、ページ識別、意味のある未知・予約コードは残します。
診断 NDJSON の `record_type` は frame_observation/semantic_observation/event_lifecycle/service_summary です。
`stream_mode`、`producer_version`、`semantic_fingerprint_version`、`bridge_protocol_version`、`session_id` を含み、
通常は `hoshiguard.session.v1` の session_start/end も出力します。利用側は未知の record type を無視してください。
旧来の通報のみの行形式が必要な場合は `hoshiguard diagnose --no-session-records` を使います。
詳細 raw・内部パスを含み得るため、診断出力をそのまま公開 API として配信しないでください。
