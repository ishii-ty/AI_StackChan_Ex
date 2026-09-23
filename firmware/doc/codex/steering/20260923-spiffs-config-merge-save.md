# SPIFFS Config Operation and Web UI Merge Save

## 目的

Core2 / CoreS3 でも、SD カードの設定ファイルを使わずに SPIFFS 上の YAML で運用できるようにする。
SPIFFS への初期設定は PlatformIO の Upload Filesystem Image（`firmware/data/` → `uploadfs`）で書き込み、以後の変更は Web UI（`/config.html`）または FTP で行う。

現状の Web UI 保存（`POST /config`）は Web UI が扱う項目だけで YAML を再生成するため、`uploadfs` で書き込んだ完全な設定ファイルのうち Web UI 非対応の項目（`stackchanApi`、`web.sd_manager`、`tts`、`stt`、`wakeword`、`moduleLLM`、`bluetooth`、`balloon` 等）が保存時に消える。これを既存 YAML へのマージ保存に変更する。

## 対象範囲

- `src/WebAPI.cpp` の `POST /config`（SPIFFS への YAML 保存処理）
- `doc/fw_design.md` の Config page 保存仕様
- SPIFFS 運用手順のドキュメント（`doc/` 配下、README からの参照）

以下は変更しない。

- 起動時の設定読込優先順位（`main.cpp` の `load_system_config()`：SD 3ファイル → SPIFFS 3ファイル → 不完全時の Config Web）。SD に設定ファイルが無ければ SPIFFS が使われるため、運用上は SD から設定ファイルを削除するだけでよい。
- 設定以外の SD 利用（アラーム MP3、FunctionCall のメモ、PhotoFrame、Pomodoro、SD Web ファイルマネージャ、SD-Updater）。
- `GET /config` の応答形式、Web UI（`incbin/config.html` / `config.js`）の画面と入力項目。
- SPIFFS 上のファイルパス（`/SC_ExConfig.yaml`、`/SC_SecConfig.yaml`、`/SC_BasicConfig.yaml`）。

## 主な変更ファイル

- `src/WebAPI.cpp`
- `doc/fw_design.md`
- `doc/spiffs_config.md`（新規：Core2 / CoreS3 / AtomS3R 共通の SPIFFS 設定書き込み手順。既存 `doc/atoms3r.md` の手順を一般化）
- `doc/atoms3r.md`、`README.md`（新規ドキュメントへの参照追加のみ）
- `doc/codex/steering/20260923-spiffs-config-merge-save.md`

## 実装方針

1. `POST /config` で各 YAML を保存する前に、SPIFFS 上の既存ファイルを `parse_yaml_file()` で `DynamicJsonDocument` に読み込む。ファイルが無い、または解析に失敗した場合は空のドキュメントから始める。
2. 読み込んだドキュメントに、Web UI が扱うキーだけを上書きする。
   - Sec: `wifi.ssid`、`wifi.password`、`apikey.aiservice`、`apikey.tts`、`apikey.stt`
   - Basic: `servo.pin.{x,y}`、`servo.offset.{x,y}`、`servo.center.{x,y}`、`servo.lower_limit.{x,y}`、`servo.upper_limit.{x,y}`、`takao_base`、`servo_type`
   - Ex: `llm.type`、`llm.enableMemory`、`llm.mcpServers`（配列は丸ごと置き換え）
   - 上記以外のキー（`llm.model` など同じ親ノード配下の非対象キーを含む）は既存値を保持する。
3. マージ後のドキュメントを YAMLDuino の `serializeYml(JsonVariant, String&)` で YAML 文字列化し、既存の保存経路（Sec は `saveSecretConfigYaml()`、Basic / Ex は `write_text_file_atomic()`）で書き込む。
4. 既存の `build_secret_yaml()` / `build_basic_yaml()` / `build_extend_yaml()` による文字列組み立ては、マージ処理に置き換えて不要になれば削除する。入力値の既定値・型チェック（`json_int_or` 等、`validate_mcp_servers()`）は維持する。
5. `DynamicJsonDocument` の容量は、テンプレート（`Copy-to-SD/` の各 YAML）をすべて埋めた場合でも収まるサイズにする（Ex は 8192 程度を目安）。容量不足（`doc.overflowed()`）を検出した場合は保存せずにエラーを返し、既存ファイルを壊さない。
6. 保存順序（Sec → Basic → Ex）とエラー時の HTTP ステータスは現状を維持する。

## 設計上の注意点

- マージ保存では YAML のコメントと元の書式は保持されない（`serializeYml` が再生成するため）。値は保持される。この点をドキュメントに明記する。
- `uploadfs` は SPIFFS 全体を上書きするため、Web UI で保存した設定、システムプロンプト（`LLMBase`）、WakeWord 登録データ等も消える。手順書に注意として記載する。
- `firmware/data/` は既に `firmware/.gitignore` で除外済み。API キー・Wi-Fi パスワードを含むため、コミットしないことを手順書に明記する。
- 起動時の設定読込バッファ（`loadConfig` の 2048）は SD 経路と同じ値であり、今回変更しない。
- 解析・マージ処理は `POST /config` 時のみ実行し、ヒープ消費は一時的なものに留める。
- 外部ライブラリは追加しない（YAMLDuino は既存依存）。

## 確認方法

- `firmware/data/` に `Copy-to-SD/` の3ファイル相当（`stackchanApi` 等を含む完全な Ex 設定）を置き、`uploadfs` 後に SD の設定ファイルを削除した状態で起動し、SPIFFS から設定が読まれること（シリアルに `Loading config from SPIFFS.`）。
- Web UI で Wi-Fi / AI Service / Servo / MCP を変更して Save した後、SPIFFS 上の YAML（FTP で取得）で次を確認する。
  - Web UI の変更が反映されていること。
  - `stackchanApi`、`web.sd_manager`、`tts`、`stt`、`llm.model`、`bluetooth`、`balloon` 等の非対象キーが保存前の値のまま残っていること。
- Restart 後、非対象キーを含めて設定が有効であること（StackChan-API 連携が動作する等）。
- SPIFFS に設定ファイルが無い状態（Config AP モード）から Web UI で保存した場合も、従来どおり3ファイルが作成され起動できること。
- `m5stack-cores3-realtime` と `m5stack-core2-realtime` のビルドが通ること。

## 戻し方

`POST /config` の保存処理を `build_*_yaml()` による再生成方式に戻す。保存ファイルの形式（YAML のキー構造）は変わらないため、データ移行は不要。
