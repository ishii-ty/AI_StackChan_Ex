# 20260703 SD カード Web ファイルマネージャ追加

## 目的

Core2 / CoreS3 で、SD カードの内容（設定 YAML、メモ等）をブラウザから閲覧・アップロード・削除できるようにする。既存 FTP サーバー（`ESP8266FtpServer`）は SPIFFS 決め打ちのため SD には未対応であり、SD の中身を確認・更新するには PC に挿し直す必要があった。今回は Web 経由のみ対応する（FTP の SD 対応はスコープ外）。

## 対象範囲

- Core2 (`m5stack-core2-realtime` など)、CoreS3 (`m5stack-cores3-realtime` など) の SD 搭載ボードのみ。
- AtomS3R（SD 非搭載、SPIFFS 使用）は対象外とし、`#if !defined(ARDUINO_M5STACK_ATOMS3R)` で機能ごと除外する。

## 主な変更ファイル

- `firmware/src/WebAPI.cpp` — `/sd/list` `/sd/download` `/sd/upload` `/sd/delete` ハンドラ追加、`init_web_server()` へのルート登録、`#include <SD.h>` 追加。
- `firmware/incbin/sdmanager.html`（新規）、`firmware/incbin/sdmanager.js`（新規）— 簡易ファイルブラウザ UI。既存 `IMPORT_FILE` マクロ（`personalize.html/js` と同方式）で埋め込む。
- `firmware/doc/fw_design.md` — 新規 Web API を追記。

## 実装方針

- 使用中の `ESP32WebServer` ライブラリは `fs::FS&` を汎用的に扱える `serveStatic()` / `onFileUpload()` / `streamFile()` / `on(uri, HTTP_POST, fn, ufn)` を既に実装済みなので、これらを利用する。新規ライブラリ導入は不要。
- `GET /sd/list?dir=/path`: `SD.open(dir)` → `openNextFile()` で列挙し `{name, isDir, size}` の JSON を返す。
- `GET /sd/download?path=/path`: `SD.open(path, "r")` → `server.streamFile(file, contentType)`。
- `POST /sd/upload?dir=/path`（multipart）: `ufn` 側で `UPLOAD_FILE_START/WRITE/END` を処理し `SD.open(finalPath, FILE_WRITE)` に書き込む。
- `POST /sd/delete`（`path` フォーム引数）: ファイルなら `SD.remove()`、ディレクトリなら `SD.rmdir()`。
- 受け取った `dir`/`path` は `/` 始まりかつ `..` を含まないことを共通ヘルパーで検証し、不正時は 400 を返す。
- 認証は既存 Web API 全体（role/memory 等）と同様に追加しない（LAN 内利用前提の既存方針を踏襲）。

## 設計上の注意点

- `main.cpp:372` の `SD.begin(...)` 成功ブロック内で `init_web_server()` が呼ばれ、`SD.end()` は呼ばれない（437行目でコメントアウト済み）ため、Web サーバー稼働中は SD がマウントされている前提がある。ただし `FunctionCall.cpp` のメモ機能が処理の都度 `SD.begin()`→`SD.end()` するため、メモ保存直後は SD が一時的にアンマウントされる場合がある。この既存の癖は今回は変更せず、新規ハンドラ側で処理の先頭に毎回 `SD.begin(GPIO_NUM_4, SPI, 25000000)` を呼んでから操作することで、他コードの状態に依存しないようにする。
- SD へのアクセスは現状すべて `loop()` タスク上の同期処理であり、SD を触る FreeRTOS タスクは存在しない。新規ハンドラも `web_server_handle_client()` から呼ばれる同期処理として実装する限り、追加のミューテックスは不要と判断する。
- 大きなファイル転送中は `loop()` がブロックされ、サーボ/lipSync/バッテリー監視等の更新が遅延しうるが、既存の FTP/Web 処理と同種の制約として許容する。
- パストラバーサル対策（`..` 拒否、`/` 始まり必須）を必ず入れる。

## 確認方法

1. `pio run -e m5stack-core2-realtime` / `pio run -e m5stack-cores3-realtime` でビルド確認。
2. 実機で `http://<device-ip>/sdmanager.html` を開き、一覧表示・ダウンロード・アップロード・削除が動作することを確認。
3. `pio run -e m5stack-atoms3r-realtime` で `/sd/*` 関連コードがコンパイル対象外になっていることを確認。
