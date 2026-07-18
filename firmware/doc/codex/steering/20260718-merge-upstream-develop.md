# 20260718 フォーク元(upstream/develop)の変更取り込み（マージ）

## 目的

フォーク元 `upstream`（`github.com/ronron-gh/AI_StackChan_Ex`）の `develop` に加わった4コミットを、当リポジトリの `develop` へ取り込む。両ブランチが分岐しており fast-forward できないため `git merge upstream/develop` を行う。本ステアリングはそのマージ（特にコンフリクト解決）の方針を定める。

取り込む upstream コミット（`007d953`→`3afa4ba`）:

| コミット | 内容 |
| --- | --- |
| `5013c4f` | Web UI から SD カード設定を構成する機能（`-DREALTIME_API` ビルドのみ） |
| `da25ab0` | Config AP / Offline モード選択画面をグラフィカルボタン化 |
| `f0cbf95` | AP モード時の SSID 末尾にランダム6桁を付加 |
| `3afa4ba` | StatusMonitor のグラフィック・UI 改善 |

当リポジトリ独自の未 upstream コミット（マージで保持する）: SD Web File Manager (`#1`)、StackChan-API 連携（ポーリング再生／カーソル方式／表情・吹き出し, `#2`）等。

## 対象範囲

- `develop` ブランチへのマージのみ。`main` は upstream/main と差分が無いため対象外。
- コード追加・改変は行わず、両サイドの既存変更を統合する（コンフリクト解決）に徹する。新機能は足さない。
- 秘密情報（APIキー・Wi-Fiパスワード）は追加しない。

## コンフリクト状況（事前 merge で確認済み）

自動マージ可能: `firmware/lib/m5stack-avatar/src/Avatar.h`、`firmware/src/StackchanExConfig.cpp` / `.h`。

要手動解決の3ファイル:

### 1. `firmware/doc/fw_design.md`（易）
両サイドが別々の節を追記しただけ。**両方の追記を残す**。

### 2. `firmware/src/main.cpp`（難）
コンフリクト2箇所。
- **グローバル変数宣言**: 両方残す。ローカル `static StackChanApiClient* stackChanApiClient`、upstream `Robot* robot = nullptr` / `bool isWebServerEnabled` / `bool isConfigPortalMode`。
- **setup() の設定読み込み・WiFi初期化**: upstream がこの領域を全面リファクタ（旧 SmartConfig 方式を廃止し、`load_system_config()`／Config AP ポータル／QR表示／フォールバック選択画面などの新ヘルパーを導入）。ローカルの変更はこの領域では小さい。

  → **upstream の新 setup() 構造をベースに採用**し、その上へローカルの以下を再適用する:
  - 通常フローの `init_web_server()` → `init_web_server(system_config.getExConfig().web.sd_manager_enabled)`。Config ポータル側の `init_web_server()` 呼び出しはデフォルト `false` のままとする（SD manager はポータルモードでは不要）。
  - 設定ロード後に `wav_init()` を挿入。
  - setup() 末尾の StackChan-API クライアント初期化ブロック（`isOffline` かつ `stackchanApi.enabled` 判定）は upstream 構造でも保持。`goto END` による分岐後も初期化まで到達することを確認する。

  コンフリクト範囲外で自動マージされる部分（そのまま残る）: ローカル追加の吹き出しヘルパー関数群と `loop()` の StackChan-API 再生ロジック、ヘッダ include 追加（`ExpressionUtil.h` / `PlayWav.h` / `api/StackChanApiClient.h`）。

### 3. `firmware/src/WebAPI.cpp`（中〜難）
upstream がファイルを大きく書き換え（+316 / −193行、config get/set API を含む再構成）。ローカルは純加算（+261行、SD Web File Manager と StackChan-API のハンドラ群＋ルート登録）。

  → **upstream 版をベースに採用**し、ローカルが追加した独立ハンドラ関数群とルート登録を移植する。関数単位ではほぼ独立しているため、upstream ベースへローカルの追加ブロックを差し込む形で対応する。include（`ArduinoJson.h` / `SD.h` / `SPIFFS.h`）は両方必要なので双方残す。

## 実装方針・注意点

- `init_web_server` はローカルが `bool enableSdManager = false` のデフォルト引数付きに変更済み（`WebAPI.h`）。upstream の引数なし呼び出しもコンパイルは通る。シグネチャ変更はローカル側のみで、upstream は `WebAPI.h` を触っていない。
- `StackchanExConfig.cpp` / `.h` は自動マージされるが、`web.sd_manager_enabled`（ローカル追加）と upstream 追加のフィールドが同一構造体に共存するため、マージ後にビルドで整合を確認する。
- コンフリクト解決は「upstream の新構造をベースに、ローカルの追加機能を載せ直す」を全ファイル共通の原則とする。
- 既存の日本語コメント・文字コードを壊さない。不要な全体再フォーマットは行わない。

## 確認方法

- マージ後、以下のビルドが通ること:
  - `pio run -e m5stack-core2-realtime`（StackChan-API・SD manager・REALTIME_API・Config Web 設定がすべて絡む主環境）
  - `pio run -e m5stack-cores3-realtime`（CoreS3 依存の確認）
  - `pio run -e m5stack-atoms3r-realtime`（AtomS3R は SPIFFS 経路・Config フロー分岐が異なるため）
- コンフリクトマーカー（`<<<<<<<` / `=======` / `>>>>>>>`）がソース・ドキュメントに残っていないこと。
- upstream の4コミットの機能（Web UI からの SD 設定、Config AP/Offline 選択画面、AP SSID のランダム6桁、StatusMonitor UI）と、ローカルの機能（SD Web File Manager、StackChan-API 連携）の双方が main.cpp / WebAPI.cpp 上に共存していること。
- `firmware/doc/fw_design.md` に両サイドの節が揃っていること。
