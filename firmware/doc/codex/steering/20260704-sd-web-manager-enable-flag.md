# 20260704 SD Web ファイルマネージャの有効/無効フラグ追加

## 目的

SD Web ファイルマネージャ（`20260703-sd-web-file-manager.md` で追加した `/sdmanager.html` と `/sd/*` API）は、現状 Core2 / CoreS3 ビルドで常時有効になっている。この機能は LAN 内無認証で SD カード全体を読み書きできるため、常用ではなく開発時のみ有効化したい。設定ファイルから ON/OFF を切り替えられるようにし、**既定は無効（opt-in）**とする。

## 対象範囲

- Core2 / CoreS3（SD 搭載ボード）。AtomS3R は SD 非搭載でルート自体が `#if !defined(ARDUINO_M5STACK_ATOMS3R)` によりコンパイル対象外のため、このフラグの影響を受けない。

## 設定ファイルの選定

- `SC_BasicConfig.yaml` は第三者ライブラリ `stackchan-arduino`（`.pio/libdeps/`）の private な `setSystemConfig()` がパースしており、独自フィールドを読むフックが無い。ライブラリ改変は更新で失われるため不可。
- `SC_ExConfig.yaml` は本リポジトリの `StackchanExConfig::setExtendSettings()` が自前でパースしているため、こちらに項目を追加する。
- ArduinoJson の `doc["web"]["sd_manager"].as<bool>()` はキーが無い場合 `false` を返すため、旧設定ファイル／フラグ未設定時は自動的に無効となり「既定 opt-in」要件を満たす。

## 主な変更ファイル

- `firmware/src/StackchanExConfig.h` — `web_s` 構造体（`sd_manager_enabled`）を追加し `ex_config_s` にフィールド追加。
- `firmware/src/StackchanExConfig.cpp` — `setExtendSettings()` でパース、`printExtParameters()` にログ 1 行追加。
- `firmware/src/WebAPI.cpp` / `WebAPI.h` — `init_web_server(bool enableSdManager)` に変更し、SD 系ルート登録をフラグで条件分岐。
- `firmware/src/main.cpp` — `init_web_server()` 呼び出しに `system_config.getExConfig().web.sd_manager_enabled` を渡す。
- `Copy-to-SD/app/AiStackChanEx/SC_ExConfig.yaml` — `web.sd_manager: false` を追加。
- `firmware/doc/fw_design.md` — 有効化フラグを追記。

## 実装方針

- 設定は `main.cpp` の `loadConfig()` で `init_web_server()` より前に読み込まれるため、フラグ値を引数で渡してルート登録の有無を制御する。
- 無効時は `/sdmanager.*` と `/sd/*` の `server.on(...)` を登録しない（アクセス時は 404）。有効/無効の切り替えには再起動が必要（既存の設定項目と同じ扱い）。

## 検証

- `pio run -e m5stack-core2-realtime` / `-cores3-realtime` / `-atoms3r-realtime` でビルドが通ること。
- `web:` セクション未記載で `/sdmanager.html` が 404（無効）。
- `web: {sd_manager: true}` で従来どおり一覧・DL・アップロード・削除が動作。
- シリアルログに `web sd_manager: false/true` が出力されること。
