# FW Design Information
Notes on FW design, etc.

- [Task](#task)
- [Mod](#mod)
  - [ESP-NOW Remote Control Mod](#esp-now-remote-control-mod)
- [Function Calling](#function-calling)
  - [Avatar Expression](#avatar-expression)
- [Wi-Fi config portal](#wi-fi-config-portal)
- [Web App](#web-app)
  - [Home page](#home-page)
  - [Config page](#config-page)
  - [Personalize page](#personalize-page)
  - [SD Card Manager page](#sd-card-manager-page)
- [Status Monitor](#status-monitor)
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

## Mod
### ESP-NOW Remote Control Mod

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

## Function Calling

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


### Realtime 音声のストリーミング再生

`RealtimeLLMBase::streamAudioDelta()` は、Realtime 系（OpenAI Realtime / Gemini Live）共通の応答音声再生処理。

- M5.Speaker のチャンネル `RT_AUDIO_PLAY_CHANNEL`（0）に固定して `playRaw()` で積む。1 チャンネルは「再生中 + 待ち」の 2 つを積めるので、`isPlaying(ch) >= 2` の間だけ待ち、鳴り終わりは待たない。鳴り終わりを待ってから次を渡すと、チャンクの境目で出力が空になり、音が途切れるため。
- バッファは `RT_AUDIO_BUF_NUM`（3）面 × `RT_AUDIO_BUF_SIZE`（100KB）を順番に使う（再生中・待ち・次のデコード先）。デコード後のサイズが収まらない delta はログを出して捨てる。
- 応答終了時は各クラスが全チャンネルの再生終了を待ち、`clearAudioBuf()` でバッファと書き込み位置を初期化する。
- リップシンク（`getAudioLevel()`）は、今鳴っている面（待ちがあれば 2 つ前、なければ 1 つ前に積んだ面）の先頭を参照する。

## Wi-Fi config portal

SmartConfig は使わず、起動時の Wi-Fi 接続元は YAML の `wifi.ssid` / `wifi.password` のみにする。

- SD に設定 YAML がある場合は SD を優先する。
- SD が無い、または主要 YAML が無い場合は SPIFFS の YAML を使う。
  - SD を使わず SPIFFS で運用する場合は、`firmware/data/` 直下に3ファイルを置いて Upload Filesystem Image（`pio run -t uploadfs`）で書き込み、SD から設定ファイルを削除する。手順は `doc/spiffs_config.md`。
- `SC_ExConfig.yaml`、`SC_SecConfig.yaml`、`SC_BasicConfig.yaml` の一式が揃っていない場合は通常機能を起動しない。
  - `REALTIME_API` ビルドでは、`SC_SecConfig.yaml` がある場合は Wi-Fi 接続だけ試し、成功したら STA 接続上で設定 Web を起動する。
  - `REALTIME_API` ビルドでは、Wi-Fi 接続できない場合、または `SC_SecConfig.yaml` も無い場合は Config AP を起動する。
  - `REALTIME_API` 以外のビルドでは、不完全な設定を画面に表示し、そのまま待機する。
- 設定一式が揃っている通常起動時は、`wifi.ssid` が空、または接続に失敗した場合に画面上で Config AP 起動または Offline 起動を選ぶ。
- Config AP は SSID `StackChanEx-Config-NNNNNN`、password `stackchan` で起動し、`http://192.168.4.1/` の QR コードを画面に表示する。SSID 末尾の 6 桁数字は AP 起動時に生成する。
- AP モード中は会話機能は offline 相当として扱うが、Web サーバーは動かし続ける。
- AtomS3R は画面が小さいため QR コードは表示せず、Wi-Fi 接続失敗時は Config AP を直接起動する。


## Web App
WebAPI.cpp のインラインアセンブラ(マクロ：IMPORT_FILE)で incbinフォルダ内のhtmlファイルやjsファイルをプログラム領域に埋め込む。

### Home page
Web アプリの入口 `/` として `home.html` を返す。`home.html` には次の 2 つの導線を置く。

- Personalize
  - `/personalize.html` に遷移する。
  - Role と Memory の管理を行う。
- Config
  - `/config.html` に遷移する。
  - Wi-Fi、API key、Realtime API 用 YAML 設定、SD から SPIFFS へのコピーを行う。

設定用 AP モードで QR に載せる URL も `/` とし、スマートフォンで開いた直後に用途を選べるようにする。

### Config page

- Wi-Fi、AI Service、Servo、MCPs (Option) の設定領域をタブで切り替えて表示する。
- タブを切り替えても未保存の入力値は保持し、Save は全タブの設定内容をまとめて保存する。
- Save、Reload、Restart と処理結果のメッセージはタブ領域の外に配置し、どのタブからでも操作可能とする。
- Reload は全タブの設定値を更新し、現在選択中のタブは維持する。

- ファイル構成
  - `incbin/config.html`
  - `incbin/config.js`
- 言語
  - ページやダイアログ内の言語は英語版のみ。
- 概要
  - 画面の入力内容を SC_SecConfig.yaml、SC_BasicConfig.yaml、SC_ExConfig.yaml のフォーマットにしてAPIで設定する。
  - 画面表示時、更新時、Reloadボタン押下時はAPIで設定値を取得して画面の入力値に反映する。
- 画面構成
  - Wi-Fi
    - SSID
    - Password (値は'*'で隠す/表示するを切り換え可能とする)
  - Realtime AI Service
    - ドロップダウン内の以下サービスから選択
      - OpenAI Realtime
      - Google Gemini Live
    - API Key　(値は'*'で隠す/表示するを切り換え可能とする)
    - Enable Memory (true or false を設定)
  - MCPs (Option)
    - MCPサーバーを最大5件設定する。未使用の入力枠は保存しない。
    - 各サーバーに Name、Disabled、URL / Host、Port を設定する。
    - Disabled は `true` / `false` のドロップダウンで選択する。
    - Name、URL / Host、Port の一部だけが入力された場合は保存エラーとする。
  - Servo
    - 以下項目を設定。詳細は [Servo Setting Details](#servo-setting-details) に記載
      - Type
      - Pin
      - Offset
      - Center
      - Lower limit
      - Upper limit
      - Enable Takao Base
  - Save ボタン
    - 画面の入力値をSC_SecConfig.yaml、SC_BasicConfig.yaml、SC_ExConfig.yaml のフォーマットにしてAPIで設定する。
  - Reload ボタン
    - APIで現在の設定を取得し、画面の入力値を更新する。
  - Restart ボタン
    - 設定内容をシステムに反映するためのシステムリセットを実行。

#### Servo Setting Details 

- Type
  - ドロップダウン内の以下タイプから選択
    - PWM : SG90PWMServo
    - SCS : Feetech SCS0009
    - DYN_XL330 : Dynamixel XL330
    - RT_DYN_XL330 : RTVersion 
    - M5_SCS : M5StackChan Servo
- Pin (x, y) 
  - 選んだTypeに応じて選択肢をドロップダウンで提示 (シリアルサーボは(x:RX, y:TX)に読み替え)。任意の値に変更も可。
    - PWM, SCS, DYN_XL330
      - Core2 PortA x:33, y:32 
      - Core2 PortB x:36, y:26
      - Core2 PortC x:13, y:14
      - CoreS3 PortA x:2, y:1
      - CoreS3 PortB x:9, y:8
      - CoreS3 PortC x:17, y:18
    - RT_DYN_XL330
      - x:6, y:7
    - M5_SCS
      - x:7, y:6
- Offset (x, y)
  - Typeによらないが、Typeを選んだときに x:0, y:0 に初期化。任意の値に変更も可。
- Center (x, y)
  - サーボの初期位置を、選択したTypeに応じて以下のように初期化。任意の値に変更も可。
    - PWM
      - x:90, y:90
    - SCS
      - x:150, y:150
    - DYN_XL330
      - x:180, y:270
    - RT_DYN_XL330
      - x:180, y:5
    - M5_SCS
      - x:150, y:85
- Lower limit (x, y)
  - サーボの可動範囲の下限を、選択したTypeに応じて以下のように初期化。任意の値に変更も可。
    - PWM
      - x:0, y:60
    - SCS
      - x:0, y:120
    - DYN_XL330
      - x:0, y:220
    - RT_DYN_XL330
      - x:90, y:-5
    - M5_SCS
      - x:0, y:0
- Upper limit (x, y)
  - サーボの可動範囲の上限を、選択したTypeに応じて以下のように初期化。任意の値に変更も可。
    - PWM
      - x:180, y:90
    - SCS
      - x:300, y:150
    - DYN_XL330
      - x:360, y:270
    - RT_DYN_XL330
      - x:270, y:15
    - M5_SCS
      - x:300, y:90
- Enable Takao Base
  - ドロップダウンで true か false を選択。Type選択時は false に初期化。

#### API
- `GET /config`: JSONで現在の設定値を返す。SPIFFS の設定ファイルが無い場合はデフォルト値またはRAM上の設定値を返す。
  - `source`: `spiffs` または `none`
  - `complete`: SPIFFS に `SC_SecConfig.yaml`、`SC_BasicConfig.yaml`、`SC_ExConfig.yaml` が揃っているか
  - `sec`: Wi-Fi と API key
  - `basic`: Servo 設定
  - `ex`: Realtime AI Service、Enable Memory、最大5件のMCPサーバー設定
- `POST /config`: POST body の JSON を検証し、SPIFFS 上の既存 `SC_SecConfig.yaml`、`SC_BasicConfig.yaml`、`SC_ExConfig.yaml` にマージして保存する。
- `POST /config/restart`: 設定反映のため再起動する。

#### 保存処理
- `SC_SecConfig.yaml` は `StackchanExConfig::saveSecretConfigYaml()` が `deserializeYml()` で構文と `wifi` / `apikey` セクションを検証する。
- 各 YAML は SPIFFS 上の既存ファイルを読み込み、Web UI が扱うキーだけを上書きして書き戻す（マージ保存）。Web UI 非対応のキー（`tts`、`stt`、`wakeword`、`moduleLLM`、`web`、`stackchanApi`、`llm.model`、`bluetooth`、`balloon` 等）は既存値を保持する。既存ファイルが無い、または解析できない場合は Web UI のキーだけで新規作成する。
  - `SC_SecConfig.yaml`: `wifi.ssid`、`wifi.password`、`apikey.aiservice`、`apikey.tts`、`apikey.stt`
  - `SC_BasicConfig.yaml`: Servo 関連（`pin`、`offset`、`center`、`lower_limit`、`upper_limit`）、`takao_base`、`servo_type`
  - `SC_ExConfig.yaml`: `llm.type`、`llm.enableMemory`、`llm.mcpServers`。MCPサーバーは最大5件とし、配列ごと置き換える。各要素に `name`、`disabled`、`url`、`port` を保存する。
- YAML は `WebAPI.cpp` の `json_to_yaml()` で再生成するため、元ファイルのコメントと書式は保持されない。YAMLDuino の `serializeYml()` は文字列をクォートせず `"0123"` 等が数値に化けるため使わず、スカラーは JSON 表記（文字列は二重引用符付き）で出力する。
- 3ファイルのマージ結果をすべて作成できた場合だけ書き込みを開始する。
- `LLM_N_MCP_SERVERS_MAX` は5とし、設定読込、Web API、MCPクライアント配列で共通の上限として使用する。YAMLに6件以上ある場合は先頭5件だけを読み込む。
- 保存後は RAM 上の `_secret_config` も更新するが、Wi-Fi 再接続は行わず Web UI から再起動を促す。
- Save成功時は、設定した値を有効にするため Restart 前に SD カードを抜くよう画面に表示する。


### Personalize page
- ファイル構成
  - incbin/personalize.html
  - incbin/personalize.js
- 言語
  - ページやダイアログ内の言語は英語版のみ。
- 画面構成
  - Role (Custom Instructions)
  - Memory

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
  - 見た目は Config page と同じデザイン（`:root` の色トークン、`main` パネル、ヘッダーの Home リンク）に揃える。
  - Home（`/`）の「SD Card Manager」リンクは既定で非表示とし、`GET /web/features` の `sdManager` が true の時だけ表示する（無効時のリンク切れ防止）。
- Web API（`src/WebAPI.cpp`）
  - `GET /web/features`: `{"sdManager":true|false}` を返す。`init_web_server()` に渡された `enableSdManager` を保持して返すだけで SD にはアクセスしない（AtomS3R は常に false）。ルートは有効化フラグに関係なく常に登録する。
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

## Status Monitor

`StatusMonitorMod` shows runtime state in tab form on the avatar sub-window.

| Tab | Contents |
| --- | --- |
| System | Existing system status: firmware version, Wi-Fi IP/MAC, heap, battery. |
| AI Service | AI service name, memory enabled/disabled, MCP server list. |

The AI Service tab reads `llm.type`, `llm.enableMemory`, and `llm.mcpServers` from `StackchanExConfig`. The old Function Call info view is not shown because it belongs to the previous Function Calling design.

The graphical tab header is drawn through `Avatar::updateSubWindowCustom()`. This keeps Avatar running and lets `StatusMonitorMod` render directly into the SubWindow during the Avatar draw cycle.
The tabs are touch targets. `StatusMonitorMod` owns tab `box_t` hit areas aligned with the drawn tab rectangles, while physical BtnA/BtnC still move to the previous/next tab.

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
  - `stackChanApiPoll` タスクが `pollIntervalMs` ごとに `GET {baseUrl}/api/messages/next[?after=<id>]` を実行する。ネットワークI/O（GET/JSONパース）のみを行い、`audioUrl`/`balloonText`/`expression` をまとめた `StackChanApiMessage` を保留として保持する。再生はメインタスクの `loop()` が `mod->isBusy()` でない時に `takePending()` で取り出して `playWavHttp()` を実行する（`alarmTimerCallbacked` と同じ、バックグラウンドで検知しメインタスクで消費するパターン）。これにより音声デバイスを触るのはメインタスクと mutexAudio で保護された Realtime 系タスクのみになり、タスク間の音声競合が構造的に発生しない。
  - `playWavHttp()` は内部で `enterMutexAudio()` を取り、Realtime 系タスクのスピーカー操作と直列化する。保留が1件ある間は次のポーリングを行わない（カーソルは受信時に進むため、保留中にGETすると保留URLを上書きして未再生のメッセージを取りこぼす）。
  - `stackchanApi.pollIntervalMs` はYAML値をそのまま使うとタイポで極端に短い値（0等）を指定した場合にポーリングタスクがタイトループしうるため、`StackchanExConfig::setExtendSettings()` で下限 1000ms にクランプする。
  - **カーソル（`after`）方式**: StackChan-API サーバーは配信状態を持たない非破壊エンドポイントであり、どこまで受け取ったかは端末が保持する。`StackChanApiClient::_lastSeenId` にレスポンスの `id`（サーバー側 AUTOINCREMENT で単調増加）を保持し、次回以降 `?after=<id>` として送る。`after` を送らないとサーバーは「直近1時間以内で最古の1件」を返し続けるため、同じ音声を再生し続けることになる。
    - カーソルは**再生の成否に関わらず受信時点で進める**。再生できないメッセージがあっても先へ進めるようにするため（同じメッセージでの無限リトライを避ける）。逆に `id` が取得できなかった場合はカーソルを進められないので**再生も行わない**。
    - カーソルは**永続化しない（RAM保持）**。再起動すると0に戻り、初回は `after` 無しでリクエストするため、直近1時間以内（サーバー側の配信ウィンドウ）のメッセージを最古から順に再生し直してから追いつく。
  - レスポンスが `200` かつ JSON に `audio.url` が含まれる場合、`{baseUrl}{audio.url}` を保留URLとして `StackChanApiClient` 内部の Mutex 保護下に保存する（`pollTask` が直接再生することはない）。`audio` を持たない（`text` のみの）メッセージは再生せずカーソルだけ進める（進めないと後続の音声メッセージが永久に届かない）。`204`（該当メッセージなし）は無視する。
  - 再生は `AudioGeneratorWAV` + `AudioFileSourceHTTPStream`（プレーンHTTP）を使い、`driver/PlayMP3.h` が公開する共有の `AudioOutputM5Speaker out` / `preallocateBuffer` を再利用する。これにより `lipSync` タスク（`robot->tts->getLevel()` 経由で `out` のバッファを参照）による口パクアニメーションも追加対応なしで動作する。
- 表情・吹き出しの反映（`expression`/`balloon`）
  - サーバーは音声と一緒に `expression`（表情名の文字列）と `balloon`（吹き出し用の短文）を任意で返せる。既存の読み上げ用 `text`（最大500文字）は FW では使わない（音声はサーバー側で合成済みの WAV を再生するため）。
  - `expression` は `src/share/ExpressionUtil.*` の `expressionFromString()` で `m5avatar::Expression` に変換する。語彙は `neutral`/`happy`/`angry`/`sad`/`doubt`/`sleepy` の6種（`src/llm/ChatGPT/FunctionCall.cpp` の `set_avatar_expression()` と同じ）。省略・未知値は `Happy` にフォールバックする。`playWavHttp(url, expression)` が再生開始時にこの表情へ、終了時に `Neutral` へ戻す（表情はステートレスで、他経路が自由に上書きしてよい）。
  - `balloon` は `truncateUtf8()`（UTF-8コードポイント単位）で表示前に10文字へ切り詰める。吹き出しの所有権・寿命管理は `main.cpp` の3つの static 関数（`apiBalloonShow` / `apiBalloonClearIfOwned` / `apiBalloonTimerExpired`）に集約し、これら以外から `stackChanApiBalloon`（永続バッファ）や消去タイマーを直接操作しない。
    - `apiBalloonShow()` はバッファへの再代入と `avatar.setSpeechText()` を必ずペアで行う唯一の場所（`Avatar::setSpeechText()` はポインタ保持でコピーしないため）。
    - 所有権は `avatar.getSpeechText() == stackChanApiBalloon.c_str()`（ポインタ同一性）で判定する。他 Mod/経路が `setSpeechText()` で上書きしていれば別ポインタになるため、`apiBalloonClearIfOwned()` はそれを消さない。この判定のために `lib/m5stack-avatar/src/Avatar.h` へ `getSpeechText()` getter を追加した（private の `speechText` を返すだけ）。
    - 再生終了後、吹き出しは10秒間表示を維持してから消える（表情は再生終了で即 `Neutral` に戻るため、寿命は表情と吹き出しで独立している）。
    - `balloon` が空/欠落のメッセージは「吹き出しなし」を意味する。再生前に自分の旧吹き出しが残っていれば `apiBalloonClearIfOwned()` で片付けてから再生し、新しい音声と無関係な旧テキストが並走しないようにする（他経路の表示は消さない）。
    - **Realtime経路との競合回避**: `RealtimeLLMBase::webSocketProcess()`（`REALTIME_API` 有効時）はアイドル時に約10msごと `setSpeechText("Please touch")` を呼ぶため、無条件だと StackChan-API の吹き出しを毎ループ上書きしてしまう。`main.cpp` の `isStackChanApiBalloonActive()` を `webSocketProcess()` から参照し、StackChan-API 吹き出し表示中はこのステータス表示をスキップする。判定は `apiBalloonOwning`（`apiBalloonShow`〜`apiBalloonClearIfOwned` の間 true）と所有権（`avatar.getSpeechText() == stackChanApiBalloon.c_str()`）の併用。`balloonClearAtMs != 0` だけでは `playWavHttp()` のブロッキング再生中（`apiBalloonShow` は再生前、`balloonClearAtMs` セットは再生後）をカバーできないため、表示区間そのものを表す専用フラグを用いている。
  - **受信サイズガード**: `pollOnce()` は `http.getSize()` が負値（Content-Length不明/chunked）または `MAX_RESPONSE_BYTES`（4KB）超なら `getString()` せず破棄する。ArduinoJson v7 の `JsonDocument` は伸縮式でコンストラクタの容量指定が実質無視されるため、`getString()` 前の実バイト数チェックでヒープ先食いを防ぐ。あわせて `deserializeJson()` に `DeserializationOption::Filter` を必須で使い、`id`/`expression`/`balloon`/`audio` 以外（未使用の `text` 最大500文字等）を読み込まない。`balloon` はさらにバイト長上限（64バイト）でも受信段階でガードし、超過分は破棄する（表示の10文字切り詰めとは別の入力サイズ対策）。
- 既知の制約
  - 認証なしの LAN 内前提（StackChan-API 側の設計に合わせている）。
  - カーソルを永続化しないため、再起動やリセットのたびに直近1時間以内のメッセージを再生し直す。
  - レスポンスの `type` は参照せず、`audio` の有無だけで再生を判断する（実運用では `speak` のみのため）。
  - サーバーとファームウェアは同時、またはファームウェア先行でリリースする必要がある。`after` を送らない旧ファームウェアと新サーバーの組み合わせは同じメッセージを無限に再生する。
  - PUSH型（WebSocket 等）は未実装。ロングポーリング化・WebSocket化は将来の拡張候補として `doc/codex/steering/20260712-stackchan-api-polling.md` に記録している。
