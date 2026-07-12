# FW Design Information
Notes on FW design, etc.

- [Task](#task)
- [ESP-NOW Remote Control Mod](#esp-now-remote-control-mod)
- [Realtime API Function Calling](#realtime-api-function-calling)
  - [Avatar Expression](#avatar-expression)
- [Web App](#web-app)
  - [Personalize page](#personalize-page)
  - [SD Card Manager page](#sd-card-manager-page)
- [Head Touch Sensor](#head-touch-sensor)
- [StackChan-API Integration](#stackchan-api-integration)


## Task

| Task name | function | Stack size [bytes] | Priority |
| --- | --- | --- | --- |
| loopTask | Arduino loop task | 8192 | 1 |
| drawLoop | Avatar control | 4 * 1024 | 2 |
| facialLoop | Avatar control | 1024 | 2 |
| lipSync | Lip Sync for avatar| 2048 | 2 |
| servo | Servo control synchronized with the avatar | 2048 | 1 |
| battery_check | Battery level check | 2048 | 1 |
| asyncTtsStreamTask | TTS streaming play | 5 * 1024 | 2 |
| webSocketLoopTask | WebSocket processing for LLM Realtime API | 6 * 1024 | 3 |
| stackChanApiPoll | StackChan-API polling (network fetch only; playback runs in main loop) | 6 * 1024 | 2 |

## ESP-NOW Remote Control Mod

初期実装では `src/mod/EspNowRemote` に Receiver 固定の Mod を追加する。

- Wi-Fi channel は `1` 固定。
- ESP-NOW 受信コールバックでは payload を固定バッファへコピーするだけにし、Serial 出力は Mod の `idle()` で行う。
- 標準 firmware の Sender から受けた場合は Arduino `esp_now` の受信 data でアプリ payload が offset `20` から始まるため、offset `20` を優先して標準 8 byte 形式として decode 表示する。8 byte だけ届く場合は offset `0` から decode する。
- ESP-NOW Remote Mod 実行中は `main.cpp` の Avatar gaze 連動 servo task を抑止し、受信した yaw/pitch を `ServoCustom::moveTo()` に渡す。
- yaw `-1280..1280` は servo x `45..-45` 度へ、pitch `0..900` は servo y `0..-30` 度へ変換する。
- payload の speed は `ServoCustom::moveTo()` の移動時間 ms として渡す。
- laser にはまだ反映しない。
- ESP-NOW Mod 実行中は Wi-Fi channel 変更により Web/FTP/Realtime API と干渉する可能性がある。
- ESP-NOW Mod 離脱時は ESP-NOW を停止し、offline mode でなければ Wi-Fi STA の再接続を最大 5 秒待つ。

## Realtime API Function Calling

### Avatar Expression

Realtime API ビルドでは、Function Calling により AI が会話中の感情に合わせて Avatar の表情を変更できる。

- 対象ビルド
  - `REALTIME_API` が定義される PlatformIO 環境。
- 関数名
  - `set_avatar_expression`
- 引数
  - `expression`: `neutral`, `happy`, `angry`, `sad`, `doubt`, `sleepy`
- 実装
  - `src/llm/ChatGPT/FunctionCall.cpp`
  - `m5avatar::Expression` に変換して `Avatar::setExpression()` を呼び出す。
  - `LLMBase.cpp` の `systemRole_realtimeAvatarExpression` で Realtime API の system instructions に利用方針を明示する。
- スコープ
  - `json_Functions` は通常 ChatGPT や Gemini Live からも参照されるため、この関数の schema と実行処理は `#if defined(REALTIME_API)` で限定する。
  - `systemRole_realtimeAvatarExpression` は Realtime 系 LLM の `load_role()` で `systemRole_memory` または `systemRole_noMemory` に追加する。

## Web App
WebAPI.cpp のインラインアセンブラ(マクロ：IMPORT_FILE)で incbinフォルダ内のhtmlファイルやjsファイルをプログラム領域に埋め込む。

### Personalize page
- ファイル構成
  - incbin/personalize.html
  - incbin/personalize.js
- 言語
  - ページやダイアログ内の言語は英語版のみ。
- 画面構成
  - Role (Custom Instructions)

#### Role (Custom Instructions)
- 構成
  - フォーム
    - ロール（カスタム指示）の入出力。
  - 設定ボタン
    - API /role_set をPOSTし、フォームの内容を設定する。
- 画面更新時の動作
  - API /role_get をPOSTし、現在設定されているカスタム指示を取得してフォームに表示する。 

#### Memory
- 構成
  - フォーム
    - 取得した記憶内容を表示。
  - Clearボタン
    - API /memory_clear をPOSTし、記憶内容を消去する。
    - 消去を実行する前に、OK/Cancelのダイアログを表示して本当に消去してよいかを確認する。
- 画面更新時の動作
  - API /memory_get をPOSTし、記憶内容を取得してフォームに表示する。

### SD Card Manager page
- 対象ボード
  - Core2 / CoreS3（SD カード搭載機のみ）。AtomS3R は SD 非搭載のため `#if !defined(ARDUINO_M5STACK_ATOMS3R)` でビルド対象から除外する。
- 有効化フラグ
  - LAN 内無認証で SD カード全体を読み書きできるため、既定では無効（opt-in）。`SC_ExConfig.yaml` の `web.sd_manager: true` を設定した時のみルートを登録する（無効時は `/sdmanager.html` `/sd/*` とも 404）。
  - 設定値は `StackchanExConfig::setExtendSettings()` が `web.sd_manager` を読み `ex_config_s.web.sd_manager_enabled` に格納し、`main.cpp` が `init_web_server(enableSdManager)` に渡してルート登録を分岐する。キー未記載の旧設定ファイルは `as<bool>()` が false を返すため自動的に無効。切り替えには再起動が必要。
- ファイル構成
  - incbin/sdmanager.html
  - incbin/sdmanager.js
- 画面構成
  - `http://<device-ip>/sdmanager.html` でディレクトリ一覧・ダウンロード・アップロード・削除ができる簡易ファイルブラウザ。
- Web API（`src/WebAPI.cpp`）
  - `GET /sd/list?dir=<path>`: 指定ディレクトリ配下のエントリを `[{name, isDir, size}, ...]` の JSON で返す。
  - `GET /sd/download?path=<path>`: 指定ファイルを `streamFile()` でダウンロードさせる。
  - `POST /sd/upload?dir=<path>`（multipart）: アップロードされたファイルを指定ディレクトリに保存する。
  - `POST /sd/delete`（form: `path`）: ファイルまたはディレクトリを削除する。
- 実装上の注意
  - 受け取った `dir`/`path` は `/` 始まりかつ `..` を含まないことを検証する（`isSafeSdPath()`）。アップロードファイル名も `/` `\` `..` を含まないことを検証する（`isSafeSdFilename()`）。
  - 各ハンドラは処理の先頭で毎回 `sdBeginRetry()`（`SD.begin(GPIO_NUM_4, SPI, 25000000)` を最大3回リトライ）を呼び、`src/llm/ChatGPT/FunctionCall.cpp` のメモ機能等が処理後に `SD.end()` している状態でも動作するようにしている。
  - アップロードは一時ファイル（`<name>.uploading`）へ書き込み、完了時に `replaceSdFile()` で本来のファイルへ置き換える。通信切断や書き込み失敗時に既存ファイルを失わないため。書き込みは `sdWriteAllRetry()` で短い書き込みをリトライする。
  - 認証は既存の Web API（role/memory 系）と同様に追加していない。LAN 内利用を前提とする。
- 既知の課題と恒久対応案
  - **SPI バス競合**: Core2/CoreS3 は SD カードと LCD が SPI バスを共有している（M5GFX が `_set_sd_spimode(bus_cfg.spi_host, GPIO_NUM_4)` で同一 SPI ホストを使う）。Arduino の `SD` ライブラリ（`sd_diskio`）は LGFX のバス管理と協調しないため、アバター描画タスク（`lib/m5stack-avatar/src/Avatar.cpp` の `drawLoop`）が SD I/O 中に SPI を使うと SD コマンドが破損し、`sd_diskio` の `Card Failed` / `Check status failed` エラーや、アップロードの 0 バイト化・500 エラーが発生することがある。
  - **現状の対処（暫定）**: `sdBeginRetry()` / `sdWriteAllRetry()` によるリトライで一時的な失敗を吸収している。開発用途では実用上問題ない程度に動作するが、競合そのものは解消していないため大きめのファイル転送では不安定さが残りうる。
  - **やってはいけない対処**: 描画タスクを `avatar.suspend()`（`vTaskSuspend`）で止める、あるいは `M5.Display.startWrite()/endWrite()` でバスをロックする方法は、いずれも別タスクから描画タスクの資源（SPI バスロック）に干渉するため、描画タスクがバスロック保持中に凍結されるとデッドロックし **HTTP が無応答になる**。実際に試して確認済みなので採用しないこと。
  - **恒久対応案**: 描画タスクに「協調的な一時停止」の仕組みを入れる。`drawLoop` の各ループ先頭（描画前でバスロックを保持していない安全地点）で「一時停止要求」フラグを確認し、要求があれば「停止完了」を通知して待機する。Web ハンドラ側は要求を立てて停止完了を待ってから SD にアクセスし、終了後に要求を解除する。これなら描画タスクは安全地点で止まるためデッドロックも競合も起きない。`lib/m5stack-avatar/`（本リポジトリ内で編集可能）の改変を伴うため、実施時は AGENTS.md の手順に従いステアリングを作成すること。

## Head Touch Sensor

`src/driver/HeadTouchSensor.*` provides the shared polling driver for the official CoreS3 head touch sensor.

- The driver owns Si12T initialization, 3ch sampling, and gesture detection.
- `AiStackChanMod` and `RealtimeAiMod` call `HeadTouchSensor::update()` from `idle()`.
- `SwipeForward` and `SwipeBackward` are treated as pet gestures.
- Each Mod keeps its own 3 second Happy expression timeout so the driver does not depend on Avatar.
- On non-CoreS3 builds, or when Si12T is not found, the driver is a no-op and normal Mod behavior continues.

## StackChan-API Integration

外部サーバー `StackChan-API`（LAN内、認証なし。別リポジトリで管理）を定期ポーリングし、音声メッセージが配信されていればダウンロードして再生する機能。`src/api/StackChanApiClient.*` が責務を持つ。

- 対象ボード
  - Core2 / CoreS3 / AtomS3R すべて。ボード非依存の機能で、ビルドフラグは無い。
- 有効化フラグ
  - 既定では無効（opt-in）。`SC_ExConfig.yaml` の `stackchanApi.enabled: true` を設定した時のみ `stackChanApiPoll` タスクを起動する。
  - 設定値は `StackchanExConfig::setExtendSettings()` が `stackchanApi.enabled` / `stackchanApi.baseUrl` / `stackchanApi.pollIntervalMs`（未指定時 5000ms）を読み `ex_config_s.stackchanApi` に格納する。キー未記載の旧設定ファイルは `as<bool>()` が false を返すため自動的に無効。
- 動作
  - `stackChanApiPoll` タスクが `pollIntervalMs` ごとに `GET {baseUrl}/api/messages/next` を実行する。ネットワークI/O（GET/JSONパース）のみを行い、音声URLは保留として保持する。再生はメインタスクの `loop()` が `mod->isBusy()` でない時に `takePendingAudioUrl()` で取り出して `playWavHttp()` を実行する（`alarmTimerCallbacked` と同じ、バックグラウンドで検知しメインタスクで消費するパターン）。これにより音声デバイスを触るのはメインタスクと mutexAudio で保護された Realtime 系タスクのみになり、タスク間の音声競合が構造的に発生しない。
  - `playWavHttp()` は内部で `enterMutexAudio()` を取り、Realtime 系タスクのスピーカー操作と直列化する。保留が1件ある間は次のポーリングを行わない（メッセージ取りこぼし防止）。
  - `stackchanApi.pollIntervalMs` はYAML値をそのまま使うとタイポで極端に短い値（0等）を指定した場合にポーリングタスクがタイトループしうるため、`StackchanExConfig::setExtendSettings()` で下限 1000ms にクランプする。
  - レスポンスが `200` かつ JSON に `audio.url` が含まれる場合、`{baseUrl}{audio.url}` を保留URLとして `StackChanApiClient` 内部の Mutex 保護下に保存する（`pollTask` が直接再生することはない）。`204`（未配信メッセージなし）は無視する。
  - 再生は `AudioGeneratorWAV` + `AudioFileSourceHTTPStream`（プレーンHTTP）を使い、`driver/PlayMP3.h` が公開する共有の `AudioOutputM5Speaker out` / `preallocateBuffer` を再利用する。これにより `lipSync` タスク（`robot->tts->getLevel()` 経由で `out` のバッファを参照）による口パクアニメーションも追加対応なしで動作する。
- 既知の制約
  - 認証なしの LAN 内前提（StackChan-API 側の設計に合わせている）。
  - PUSH型（WebSocket 等）は未実装。ロングポーリング化・WebSocket化は将来の拡張候補として `doc/codex/steering/20260712-stackchan-api-polling.md` に記録している。
