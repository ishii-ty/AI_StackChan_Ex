# Wi-Fi Connection Timeout Extension

## 目的

起動時の Wi-Fi 接続で、AP から `Association refused temporarily, comeback time 1024 mSec`（PMF / 802.11w 有効 AP がリセット直後の再接続を一時拒否する挙動）が返ると、`Wifi_connection_check()` の 5 秒タイムアウトに間に合わず接続失敗扱いになり、Config AP / Offline の選択画面で停止する。設定（SPIFFS の YAML）は正しく読めているため、待ち時間を延ばして一時拒否からの回復を待てるようにする。

## 対象範囲

- `src/main.cpp` の `Wifi_connection_check()` のタイムアウト値のみ。
- 呼び出し元（`connect_wifi_from_yaml()` → 通常起動 / 設定不完全時の Config Web 経路）は変更しない。
- フォールバック（Config AP / Offline 選択、AtomS3R は Config AP）の挙動は変更しない。

## 主な変更ファイル

- `src/main.cpp`
- `doc/codex/steering/20260923-wifi-connect-timeout.md`

## 実装方針

1. タイムアウト 5000ms を名前付き定数（例: `WIFI_CONNECT_TIMEOUT_MS = 20000`）にして 20 秒に延ばす。コメントに延長理由（AP の一時拒否 comeback を待つため）を残す。
2. 1 秒ごとの `.` 表示（Display / Serial）はそのまま。接続できれば即座に抜けるため、正常時の起動時間は変わらない。

## 設計上の注意点

- SSID 誤り・AP 不在時は、フォールバック画面が出るまで最大 20 秒（従来 5 秒）かかる。
- `WiFi.begin()` の再試行は今回入れない（ESP-IDF 側が comeback time 後に自動で再アソシエーションするため、待つだけで回復する想定）。20 秒でも失敗が続く場合に別途検討する。

## 確認方法

- 実機（CoreS3）でリセットを繰り返し、`Association refused temporarily` が出ても接続成功（シリアルに `Successfully connected to Wi-Fi using YAML settings.` と IP）まで進むこと。
- 誤った SSID で約 20 秒後にフォールバック画面が出ること。
- `m5stack-core2-realtime`、`m5stack-cores3-realtime`、`m5stack-atoms3r-realtime` のビルドが通ること。

## 戻し方

定数を 5000 に戻す。
