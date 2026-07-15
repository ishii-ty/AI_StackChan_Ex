# 20260716 StackChan-API連携のカーソル(after)方式対応

## 目的

`../StackChan-API` 側で `GET /api/messages/next` の仕様が変わった（同リポジトリのコミット `f90f91e`「配信フラグを廃止し INDEX カーソル方式のポーリングへ」）。

- 変更前: サーバーがGETのたびに対象を `delivered` に更新する破壊的エンドポイント。端末は配信位置を覚える必要が無かった。
- 変更後: サーバーは配信状態を一切持たない非破壊・読み取り専用エンドポイント。配信位置は端末が持つカーソル `?after=<id>` に移った。複数機器が同時にポーリングしてもメッセージが端末間で分配されず全機に届くようにするための変更。

現行実装（`20260712-stackchan-api-polling.md` で追加）は `after` を送らず、レスポンスの `id` も読んでいない。このままサーバーを更新すると、サーバーは毎回「直近1時間以内で最古の ready 1件」を返し続けるため、**同じ音声を `pollIntervalMs` ごとに永久に再生し続ける**。受信した `id` を保持して次回以降 `after` に載せ、各メッセージを1回だけ再生するようにする。

## 対象範囲

- Core2 / CoreS3 / AtomS3R すべて（前回同様ボード非依存。ビルドフラグは追加しない）。
- YAML設定の追加・変更は無し（`stackchanApi.enabled` / `baseUrl` / `pollIntervalMs` のまま）。再生経路（`driver/PlayWav.*`）と `main.cpp` のフックも変更しない。

## サーバー側の仕様（`StackChan-API/server/routes/messages.ts:160-199` で確認）

- `GET /api/messages/next?after=<id>` — `after` は省略可。非負整数でなければ **400** `{"error": "after must be a non-negative integer"}`。
- 該当メッセージなし → **204**（ボディ無し）。
- 該当あり → **200**。`id`(number, AUTOINCREMENT で単調増加・1始まり) / `type`(string) / `text`(string|null) を返し、音声キャッシュがある時のみ `audio`(`url` / `format` / `sampleRate`) が付く。`audio.url` は `baseUrl` からの相対パス。
- `after` 未指定は `m.id > COALESCE(NULL, 0)` すなわち `m.id > 0` と等価で、**直近1時間以内の最古の1件**を返す（「最新」でも「なし」でもない）。
- 204になる条件は3つ: ①1時間の配信ウィンドウ外（`after` の有無に関係なく適用）②カーソルより新しいものが無い ③pendingバリア（先行メッセージが合成中の間は id順＝配信順 を守るため後続を止める）。

## 主な変更ファイル

- `firmware/src/api/StackChanApiClient.h` — `uint32_t _lastSeenId` を追加。
- `firmware/src/api/StackChanApiClient.cpp` — `pollOnce()` のURL組み立てに `?after=` を追加、レスポンスの `id` でカーソルを進める。`pollTask()` の「保留中はGETしない」コメントの根拠を書き直す。
- `firmware/doc/fw_design.md` — `## StackChan-API Integration` をカーソル方式に合わせて更新。

## 実装方針

- `_lastSeenId` は `pollTask`（`pollOnce()`）だけが読み書きするのでMutexは不要。`_pendingAudioUrl` を守る `_pendingMutex` とは保護対象が別である旨をコメントで明示する。
- **カーソルは永続化しない（RAM保持）**。再起動すると0に戻り、直近1時間以内のメッセージを最古から順に再生し直してから追いつく。NVS等への保存は行わない。
- **起動直後（カーソル0）は特別扱いしない**。初回は `after` 無しでリクエストし、返ってきたものをそのまま再生する。バックログを読み飛ばす処理は入れない。
- **カーソルは受信時に即進める**。JSONパースで `id` が取れた時点で更新する（at-most-once）。再生成否は問わないので、再生できないメッセージがあっても無限ループしない。
- **`audio` が無い（`text` のみの）メッセージは再生せずカーソルだけ進める**。進めないと後続の音声メッセージが永久に届かなくなる。
- `id` が取れなかった場合は**再生もしない**。カーソルが進まないまま再生すると同じ音声を再生し続ける（＝今回直そうとしている不具合そのもの）になるため。
- 履歴 `GET /api/messages` は使わない。`/next` だけで完結する。履歴は全文付き200件を返して重いうえ、先頭が `pending`/`failed` 行だとその id にカーソルを合わせて未合成メッセージを永久に取りこぼす。
- `type` は現状どおり参照しない（実運用では `speak` のみ）。既知の制約として `fw_design.md` に残す。

## 設計上の注意点

- `pollTask()` の「保留中の音声URLがある間は次のGETを行わない」ガードは**残すが理由が変わる**。旧: GETするとサーバー側でdelivered扱いになり取りこぼす。新: カーソルが受信時に進むため、保留中に次をGETすると `_pendingAudioUrl` を上書きして未再生のメッセージを取りこぼす。コメントを更新しないと誤った記述が残る。
- ArduinoJsonはv7（`platformio.ini:72`）で `JsonDocument` が伸縮式のため、`text` が最大500文字でも既存の `DynamicJsonDocument(2048)` の容量指定は実質無視される。容量まわりの変更は不要。
- `after` に載せるのは `_lastSeenId`（`uint32_t`）なので常に非負整数であり、400が返り続ける状態には陥らない。専用のリカバリは入れない。
- **デプロイ順序**: サーバーとファームは同時、またはファーム先行でリリースする必要がある。旧ファーム（`after` を送らない）＋新サーバーの組み合わせは同じメッセージを無限に再生する。逆に新ファーム＋旧サーバーは、旧サーバーが `after` を無視してdelivered更新するだけなので実害なく動く。

## 確認方法

- `pio run -e m5stack-core2-realtime` / `-cores3-realtime` / `-atoms3r-realtime` でビルドが通ること。
- `POST /api/messages` で1件投入 → 1回だけ再生され、`StackChanApi: audio message received:` が1回だけ出ること（現状の不具合＝これが `pollIntervalMs` ごとに繰り返される、が直っていること）。その後は204が続き何も出ないこと。
- 連続で3件投入 → 3件が id順に1回ずつ再生されること。
- 再起動 → 直近1時間以内のメッセージを最古から順に再生し直し、追いついたら止まること（RAM保持の期待動作）。
- サーバー側のログで、初回が `?after` 無し・2回目以降が `?after=<直前のid>` になっていること。
