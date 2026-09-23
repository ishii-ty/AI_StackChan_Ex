# SD Web File Manager UI Alignment and Home Link

## 目的

SD Web ファイルマネージャ（`/sdmanager.html`）の見た目を既存の Web UI（`home.html` / `config.html`）と揃え、Home（`/`）から遷移できるようにする。あわせて SD マネージャ画面から Home へ戻るリンクを付ける。

## 対象範囲

- `incbin/sdmanager.html`：スタイルを `config.html` と同じデザイントークン（`:root` の `--bg` `--panel` `--line` `--primary` 等）・レイアウト（`main` パネル、`header` に h1 + Home リンク、ボタン形状、狭幅時の表示）に置き換える。DOM の id（`breadcrumb` `upButton` `fileList` `uploadForm` `uploadInput` `status` `error`）は維持し、`sdmanager.js` のロジックは原則変更しない（削除ボタン等のクラス名調整程度）。
- `incbin/home.html`：ナビに「SD Card Manager」リンクを追加する。SD マネージャは `web.sd_manager: true` の時のみルート登録される（無効時・AtomS3R は 404）ため、リンクは既定で非表示にし、有効な場合のみ表示する。
- `src/WebAPI.cpp`：Home が SD マネージャの有効/無効を判定するための軽量 API `GET /web/features` を追加する。応答は `{"sdManager":true|false}`。`init_web_server()` に渡された `enableSdManager`（AtomS3R では常に false）をファイルスコープの static に保持して返す。SD へのアクセスは行わない。
- `doc/fw_design.md`：`/web/features` と Home からの導線を追記する。

以下は変更しない。

- `/sd/*` API の仕様、SD マネージャの有効化条件（`web.sd_manager` opt-in）、AtomS3R での無効化。
- `config.html` / `personalize.html` の見た目（`personalize.html` は旧スタイルのままだが今回の対象外）。

## 主な変更ファイル

- `incbin/sdmanager.html`
- `incbin/sdmanager.js`（クラス名調整が必要な場合のみ）
- `incbin/home.html`
- `src/WebAPI.cpp`
- `doc/fw_design.md`
- `doc/codex/steering/20260923-sdmanager-ui-home-link.md`

## 実装方針

1. `home.html` に `<a id="sdManagerLink" href="/sdmanager.html" hidden>SD Card Manager</a>` を追加し、インライン script で `fetch("/web/features")` → `sdManager` が true なら `hidden` を外す。取得失敗時は非表示のまま（旧ファームや通信失敗でも壊れない）。説明文も「Config / Personalize / SD Card Manager」を含む文に合わせる。
2. `WebAPI.cpp` に `static bool s_sdManagerEnabled = false;` を置き、`init_web_server()` の `#if !defined(ARDUINO_M5STACK_ATOMS3R)` 内で `enableSdManager` を代入する。`handle_web_features()` で JSON を返し、`server.on("/web/features", HTTP_GET, ...)` を APIs に登録する。
3. `sdmanager.html` を `config.html` のスタイルに合わせて書き直す。一覧テーブルは横幅に収まるよう名前列を折り返し、削除ボタンは `--danger` 色、アップロードは `button.primary`。狭幅（680px 以下）では `config.html` と同じく全面表示にする。

## 設計上の注意点

- `/web/features` は機能の有無だけを返し、設定値や秘密情報は含めない。無認証だが既存の `/sdmanager.html` が 404 か否かと同じ情報しか出さない。
- ページは `.rodata` に埋め込まれるため、HTML サイズ増は数 KB 程度に留める。外部 CSS/JS は使わない（オフライン動作のため）。
- incbin の依存関係は `20260719-incbin-build-dependencies.md` の仕組みでビルド時に再埋め込みされる前提。

## 確認方法

- `web.sd_manager: true` で起動し、`/` に「SD Card Manager」が表示され遷移できること、SD マネージャで一覧・移動・DL・アップロード・削除が従来どおり動くこと、Home リンクで戻れること。
- `web.sd_manager: false`（またはキー無し）で `/` にリンクが表示されないこと、`/web/features` が `{"sdManager":false}` を返すこと。
- スマホ幅で横スクロールが出ないこと。
- `m5stack-core2-realtime`、`m5stack-cores3-realtime`、`m5stack-atoms3r-realtime` のビルドが通ること。

## 戻し方

`home.html` のリンクとスクリプト、`/web/features` ルートを削除し、`sdmanager.html` を以前のスタイルに戻す。永続データや設定形式の変更は無い。
