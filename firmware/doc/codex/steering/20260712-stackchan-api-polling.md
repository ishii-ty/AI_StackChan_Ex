# 20260712 StackChan-API連携（音声メッセージのポーリング再生）

## 目的

`../StackChan-API`（LAN内で動く別サーバー、Bun + Express + SQLite）は `GET /api/messages/next` で未配信メッセージを1件返す設計になっている（無ければ204）。レスポンスに `audio.url` が含まれる場合、そのURLからWAV音声をダウンロードして再生する機能を firmware に追加する。設定（APIサーバーのURL・ポーリング間隔・有効/無効）はYAML化する。

PUSH型（WebSocket/SSE/MQTT）も検討したが、StackChan-API側は現状ポーリング専用API（認証なし、LAN内前提）であるため、今回は短間隔ポーリングのみを実装する。ロングポーリング化・WebSocket化は将来の拡張候補として記録に留める（本ファイル末尾参照）。

## 対象範囲

- Core2 / CoreS3 / AtomS3R すべて（機能はボード非依存。設定はSD/SPIFFSいずれのYAML経路でも共通の `StackchanExConfig::setExtendSettings()` を通る）。
- ビルドフラグは追加しない。YAML設定の `stackchanApi.enabled`（既定false、opt-in）のみで有効/無効を切り替える。

## 主な変更ファイル

- `firmware/src/StackchanExConfig.h` / `.cpp` — `stackchan_api_s`（`enabled` / `baseUrl` / `pollIntervalMs`）を追加し `ex_config_s` にフィールド追加。`setExtendSettings()` でパース、`printExtParameters()` にログ追加。
- `Copy-to-SD/app/AiStackChanEx/SC_ExConfig.yaml` — `stackchanApi:` セクションを追加（既定 `enabled: false`）。
- `firmware/src/driver/PlayWav.h` / `.cpp`（新規） — `PlayMP3.h` の `out` / `preallocateBuffer` / `preallocateBufferSize` を再利用したWAV再生（`AudioGeneratorWAV` + `AudioFileSourceHTTPStream`、プレーンHTTP）。`wav_init()` と `playWavHttp(const String& url)` を提供。
- `firmware/src/api/StackChanApiClient.h` / `.cpp`（新規） — `GET {baseUrl}/api/messages/next` を定期実行し、`audio.url` があれば保留URLとして保持する（再生はしない）。専用FreeRTOSタスクで実行。`takePendingAudioUrl()` で保留URLを取り出せる。
- `firmware/src/main.cpp` — `wav_init()` 呼び出し追加、`stackchanApi.enabled` に応じた `StackChanApiClient` の起動追加。`loop()` の `mod->idle()` 直後で保留URLを取り出し `playWavHttp()` を実行する処理を追加。
- `firmware/AGENTS.md` — 責務分担表に `src/api` を追記。
- `firmware/doc/fw_design.md` — 新タスク・新設定項目を追記。

## 実装方針

- WAV再生は既存の `playMP3()`／`playMP3SD()`／`playMP3SPIFFS()`（`firmware/src/driver/PlayMP3.cpp`）と対になる構成にし、`avatar.setExpression(Happy/Neutral)` と `servo_home` の切り替えも既存パターンに合わせる。`out`（`AudioOutputM5Speaker`）を共有するため、`lipSync` タスク（`main.cpp`、`robot->tts->getLevel()` 経由）による口パクは追加対応なしでそのまま動く。
- `playWavHttp()` はメインタスク（`loop()`）から呼ばれるが、Realtime系タスクのスピーカー操作と直列化するため `playMP3()` 系と同様に `enterMutexAudio()`/`exitMutexAudio()`（`firmware/src/share/Mutex.*`）による排他を行う。
- `stackChanApiPoll` タスク（`StackChanApiClient::pollTask`/`pollOnce`）はネットワークI/O（`GET {baseUrl}/api/messages/next` + JSONパース）のみを行う。`audio.url` を受信したら `{baseUrl}{audio.url}` を保留URLとして内部の `SemaphoreHandle_t`（`xSemaphoreCreateMutex()`）で保護した `String _pendingAudioUrl` / `bool _hasPendingAudioUrl` に保存するだけで、自ら再生は行わない。保留が1件ある間は次の `GET` を行わない（サーバー側でdelivered扱いになりメッセージを取りこぼすため）。
- 再生はメインタスク（`main.cpp` の `loop()`）が担う。`mod->idle()` の直後に `stackChanApiClient->takePendingAudioUrl(pendingAudioUrl)` で保留URLを取り出し（`mod->isBusy()` でない時のみ）、`playWavHttp()` を呼ぶ。これは `FunctionCall.cpp` の `alarmTimerCallbacked` を `AiStackChanMod::idle()`/`RealtimeAiMod::alarmEventHandler()` が消費するのと同じ「バックグラウンドタスクが検知しメインタスクで消費する」パターンであり、音声デバイスに触れるタスクをメインタスクと（既に `enterMutexAudio()`/`exitMutexAudio()` で保護済みの）Realtime WebSocketタスクだけに限定する。
- HTTP GETは `WebVoiceVoxTTS::https_get()` と同様の `HTTPClient` パターンを踏襲するが、StackChan-APIはプレーンHTTPのため `WiFiClientSecure`／ルートCAは不要。
- `stackchanApi` セクションが無い旧YAMLでは `enabled` が既定 `false` になり、機能は無効のまま（`web.sd_manager` と同じ opt-in 方式）。
- `stackchanApi.pollIntervalMs` はYAML値をそのまま使うとタイポで極端に短い値（0等）を指定した場合にポーリングタスクがタイトループしうるため、`StackchanExConfig::setExtendSettings()` で下限 1000ms にクランプする（このクランプ自体は独立した正しい修正であり、下記のレビュー対応履歴・根本対策の後も維持している）。

## レビュー対応履歴（初期実装時、いずれも旧設計＝ポーリングタスクが直接再生する方式の下での対応）

- 1回目のCodexレビュー：ポーリングタスクは `get_current_mod()->isBusy()` が true の間は再生をスキップすべきところ、`RealtimeAiMod` にしか `isBusy()` が実装されておらず非Realtimeモードでポーリング再生が録音/TTS再生に割り込みうる欠陥を指摘された。`AiStackChanMod` にも `isBusy()`（STT→LLM→TTS処理中・アラーム再生中に true を返す `sttChatBusy` フラグ）を実装して対応した。
- 2回目のCodexレビュー：逆方向（API音声再生中にユーザー操作でSTTが割り込む）を指摘された。`driver/PlayWav.h/.cpp` に `isWavHttpPlaying()`（`playWavHttp()` がMic/Speakerを握っている間 true）を追加し、`AiStackChanMod::STT_ChatGPT()` が録音開始前に確認してスキップ、アラーム再生も再生中は次の `idle()` まで待つよう対応した。
- 3回目のCodexレビュー：スケジューラ（`run_schedule()` → `ScheduleReminder::run()`/`sched_fn_announce_time()`等が `playMP3SD()`/`robot->speech()` を呼ぶ経路）が同じ穴を持つと指摘された。`AiStackChanMod::idle()` の `run_schedule()` 呼び出しをアラームと同じパターンで包んで対応した。同レビューで `pollIntervalMs` の下限クランプも指摘され対応した（この修正のみ根本対策後も維持）。
- 4回目のCodexレビュー：`pollTask` 入口の `isBusy()` チェックから HTTP GET 完了までの間にユーザーが録音を開始しうる（TOCTOU）点を指摘された。`pollOnce()` で `playWavHttp()` 直前に再度 `isBusy()` を確認するチェックを追加して対応した。
- 5回目のCodexレビュー：ポーリングタスクの `get_current_mod()` とメインタスクのフリック `change_mod()`（`std::deque` の pop/push）が無同期で衝突しUBになる点を指摘された。`ModManager.cpp` に Mutex を追加して対応した。

## 根本対策（5回のレビュー対応を置き換え）

5回のレビュー指摘（busy未実装、STT割り込み、スケジューラ競合、TOCTOU、ModManager競合）はすべて「独立タスクが音声再生を直接実行する」という同一の根本原因に由来していた。対症療法のフラグでは競合窓を原理的に閉じられないため、再生をメインタスク（`loop()`）へ移すアーキテクチャに変更した。ポーリングタスクはネットワークI/Oのみを行い保留URLを保持し、`loop()` が `isBusy()` でない時に取り出して再生する（`alarmTimerCallbacked` と同じパターン）。これに伴い `sttChatBusy` / `isWavHttpPlaying()` / `pollOnce()` 内のTOCTOU再チェック / `ModManager` の Mutex を撤去し、`AiStackChanMod`・`RealtimeAiMod`・`ModManager`・`PlayWav` は機能追加前の形に戻した。`pollIntervalMs` の下限クランプ（3回目のレビュー対応）のみ、独立した正しい修正として維持している。

## 検証

- `pio run -e m5stack-core2-realtime` / `-cores3-realtime` / `-atoms3r-realtime` でビルドが通ること。
- `stackchanApi: {enabled: false}`（または未記載）で `stackChanApiPoll` タスクが起動しないこと。
- `stackchanApi: {enabled: true, baseUrl: "http://<StackChan-APIのIP>:<port>"}` を設定したSDカードで、`StackChan-API` に `POST /api/messages` でテキストを投入 → 実機がポーリングで検知しWAVをダウンロード・再生すること（シリアルログと実機音声で確認）。
- 会話中（`mod->isBusy()==true`）に音声メッセージを受信しても再生に割り込まず、保留のまま待機し、busyでなくなった後の `loop()` で再生されること。

## 将来のPUSH化（今回は未実装、設計メモ）

- **ロングポーリング**: StackChan-API側の `GET /api/messages/next` を最大N秒（例:25秒）保持してから応答する方式に変更。firmware側は `HTTPClient::setTimeout()` を伸ばすだけで済み新規ライブラリ不要。API側（Express）の変更のみで体感速度が上がる、費用対効果の高い次ステップ。
- **WebSocket**: `RealtimeLLMBase.cpp` が使う `arduinoWebSockets`（`[realtime_api]` セクションのlib_dep）を流用可能。ただし非`REALTIME_API`ビルドでは新規lib_dep追加・再接続処理・専用タスク管理が必要になり、API側にもWebSocketサーバー実装が必要で実装コストが最も高い。
- **MQTT**: ブローカーという新規インフラが必要になり、StackChan-API（Express+SQLiteのみ）の設計から乖離するため非推奨。
