# 20260718 StackChan-API連携: 表情・吹き出しメッセージの反映

## 目的

現行の StackChan-API 連携（`20260712-stackchan-api-polling.md` / `20260716-stackchan-api-cursor.md`）は、
サーバーをポーリングして音声（WAV）を再生するだけで、再生中の表情は `playWavHttp()` が固定で `Happy` にし、
吹き出しには何も表示しない。

本作業では、サーバーが音声と一緒に「表情の指示」と「吹き出しに出す短いテキスト」を配信できるようにし、
端末がそれを再生と同期して反映する。「音声を鳴らしつつ、指定した表情になり、吹き出しに一言表示する」を実現する。

読み上げ用の既存 `text`（最大500文字）は FW では使わない（音声はサーバー側で合成済みの WAV を再生）。
吹き出し用テキストは読み上げ文とは別の短いフィールドで受け取る。

## 対象範囲

- Core2 / CoreS3 / AtomS3R すべて（従来同様ボード非依存。ビルドフラグは追加しない）。
- YAML設定の追加・変更は無し（`stackchanApi.enabled` / `baseUrl` / `pollIntervalMs` のまま）。
- カーソル(after)方式・「保留中はGETしない」ガードは現行のまま維持する。

## 決定事項（ユーザー確認済み）

- **表情の継続**: 再生中だけ指定表情を出し、再生終了後は `Neutral` へ戻す（既存挙動を踏襲）。
- **吹き出しの消去**: 音声再生終了後 **10秒間** 表示して残し、その後クリア。次のメッセージが来たら上書き。
- **文字数超過**: FW 側で **10文字**（全角=1文字カウント / UTF-8コードポイント単位）に切り詰める。
- **吹き出しの所有権**: 消去は常に「API吹き出しが今も表示中のとき」だけ行う（他経路が上書きしていたら触らない）。
- **空/欠落 balloon の意味**: 「このメッセージには吹き出しが無い」。再生前に**自分の**旧API吹き出しが残っていれば
  所有権チェック付きで消してタイマーも無効化する（新しい音声と無関係な旧テキストを並走させない）。
  他経路の表示は消さない。
- **入力サイズ**: 表示の10文字制限とは別に、受信段階でレスポンスサイズと `balloon` バイト長の上限で過大入力を弾く。

## 表情パターン（全6種・固定）

ライブラリの enum `{ Happy, Angry, Sad, Doubt, Sleepy, Neutral }`（`lib/m5stack-avatar/src/Expression.h:9`）が上限。
API では文字列で指定し、既存 Realtime Function Calling（`src/llm/ChatGPT/FunctionCall.cpp:62`）と同じ語彙に揃える:
`neutral` / `happy` / `angry` / `sad` / `doubt` / `sleepy`。省略時・未知値は `Happy` へフォールバック。

## サーバー側への依頼（別リポジトリ `StackChan-API`。本作業はFW側のみ実装）

`GET /api/messages/next` の `action` レスポンス（`server/routes/messages.ts:181-187`）に2フィールド追加:

1. `expression`（string, 任意）: 上記6値のいずれか。省略可。
2. `balloon`（string, 任意）: 吹き出し用の短文。既存 `text`(最大500) とは別フィールド。推奨10文字。省略可。

サーバー実装: `messages` テーブル（`server/db.ts:32-40`）に `expression`/`balloon` カラム追加、
`POST /api/messages` で受理・保存、`getNext`（`server/routes/messages.ts:135-149`）SELECT と `/next` 応答に追加。
両フィールドとも任意で、省略すれば旧挙動（表情 Happy・吹き出しなし）と同じ。リリース順は問わない。

## 主な変更ファイル

- `firmware/src/api/StackChanApiClient.h` / `.cpp` — 受信データを構造体化し `expression`/`balloon` をパース・保持。
- `firmware/src/driver/PlayWav.h` / `.cpp` — `playWavHttp()` に `expression` 引数追加、表情反映、終了時 Neutral 復帰（吹き出しは扱わない）。
- `firmware/src/main.cpp` — 吹き出し消去タイマー（所有権チェック付き）・UTF-8 10文字切り詰め・永続String・呼び出し更新。
- `firmware/src/share/ExpressionUtil.h` / `.cpp`（新規）— 文字列→Expression 共通ヘルパ + UTF-8切り詰めヘルパ。
- `firmware/lib/m5stack-avatar/src/Avatar.h` / `.cpp` — 現在の吹き出し文字列ポインタを返す getter を1つ追加（所有権チェック用）。
- `firmware/doc/fw_design.md` — StackChan-API Integration 節に追記。

## 実装方針

### StackChanApiClient
- 保留データを URL 単体から構造体へ拡張:
  ```cpp
  struct StackChanApiMessage { String audioUrl; String balloonText; String expression; };
  ```
  `_pendingAudioUrl`/`_hasPendingAudioUrl` を `StackChanApiMessage _pending` + `bool _hasPending` に置換。
- `takePendingAudioUrl(String&)` → `takePending(StackChanApiMessage&)`（呼び出しは main.cpp の1箇所のみ）。
- `pollOnce()` で `doc["expression"]` / `doc["balloon"]` を読み保留へ格納。`audio` 無しメッセージは
  従来どおり再生せずカーソルだけ進める（吹き出しのみ表示は今回スコープ外。音声同期が前提）。
- **入力サイズのガード（Finding対応）**:
  - `deserializeJson` に `DeserializationOption::Filter` を **必須**で入れ、`id`/`expression`/`balloon`/`audio` のみ
    抽出する（未使用の `text` 最大500文字を読まない＝既存よりヒープ削減）。「任意最適化」から「必須」へ格上げ。
  - **`getString()` 前に実バイト数で上限を掛ける（Finding対応）**。`http.getSize()` は Content-Length が無い/
    chunked だと負値（長さ不明）を返し、これを許すと `getString()` が巨大本文を丸ごと `String` に確保して
    ヒープを先食いしリセットする。よって `int len = http.getSize();` が **`len < 0`（不明/chunked）または
    `len > MAX_RESPONSE_BYTES`（例 4KB）なら getString せず破棄** する（安全側に倒す。StackChan-API の JSON 応答は
    Content-Length 付きなので正常系は通る）。より厳密には `http.getStreamPtr()` から最大 `MAX_RESPONSE_BYTES+1`
    バイトだけ読む bounded reader にして超過検出で破棄する方式も可（望ましい）。Filter はパース段階の対策で
    ヒープ先食いは防げないため、この受信サイズ制限と併用する。
  - `balloon` は保存前にバイト長でも上限（例 64バイト＝全角約21文字ぶんの余裕）を設け、超過分は捨てる。
    10文字切り詰めは表示制約、バイト長上限は入力サイズ制約として二重に持つ。

### 表情変換の共通化
- `src/share/ExpressionUtil.*`（新規）に
  `m5avatar::Expression expressionFromString(const String& s, m5avatar::Expression fallback)` を置く。
- 本機能はこれを使う。`FunctionCall::set_avatar_expression()`（`FunctionCall.cpp:409-440`、`#if REALTIME_API`内）の
  同ロジックを本ヘルパ利用へ置き換える重複解消は望ましいが任意（REALTIME_API 経路に触れるため別扱い可）。
- UTF-8 コードポイント単位の切り詰め `truncateUtf8(const String&, size_t maxChars)` も同ヘルパに置く。

### 吹き出し所有権チェック用の getter（Finding対応）
- `avatar.setSpeechText()` は14ファイル（各 Mod・LLM・STT 経路）が直接呼んでおり集中管理されていない。
  再生後の消去タイマーが無条件に `setSpeechText("")` すると、10秒窓中に他経路が表示した別テキストを消す。
- vendored な `lib/m5stack-avatar` の `Avatar` に、現在保持中の吹き出し文字列ポインタを返す getter を1つ追加:
  ```cpp
  const char* getSpeechText() const { return speechText; }
  ```
  （`Avatar.h:25` の private `speechText` を読むだけ。1メソッド追加でupstream差分は最小。）
- 消去時に `avatar.getSpeechText() == stackChanApiBalloon.c_str()`（ポインタ同一性）で
  「今表示中の吹き出しが自分のものか」を判定する。他経路が上書きしていれば別ポインタになり、消さない。

### PlayWav
- `playWavHttp(const String& url, const String& expression)` に拡張（**吹き出しは扱わない**）。
- 開始時（現 `PlayWav.cpp:36`）: `avatar.setExpression(expressionFromString(expression, Expression::Happy));`。
- 終了時（現 `PlayWav.cpp:55`）: `avatar.setExpression(Expression::Neutral);`。
- **吹き出しの `setSpeechText` は playWavHttp では呼ばない**。所有権管理を1箇所（main.cpp loop）に集約するため。
  表情はステートレス（各経路が自由に上書きしてよい）なので従来どおり playWavHttp が持つ。

### main.cpp — 吹き出し状態は3操作に集約（Finding対応の根本策）

これまでのレビュー指摘（無条件クリアが他表示を消す／バッファ使い回しが表示中ポインタを壊す／空balloonの扱いが
互換仕様と矛盾）は、いずれも「吹き出し状態への操作ルールが loop 内に分散し、特例を継ぎ足すたびに整合が崩れる」
ことが根因。そこで **API吹き出しの状態（永続バッファ + 消去タイマー）に触ってよいのは次の3つの static 関数だけ**
とし、loop はこれらを呼ぶだけにする。

```cpp
// 前提: avatar.setSpeechText() はコピーせずポインタ保持（lib/m5stack-avatar/src/Avatar.cpp:209-211）。
// 所有権 = 「Avatar が保持するポインタが stackChanApiBalloon.c_str() と同一」(avatar.getSpeechText() で判定)。
static String   stackChanApiBalloon;   // Avatar に渡す永続バッファ兼所有権基準
static uint32_t balloonClearAtMs = 0;  // 0 = タイマー無効

// (1) 表示: バッファ再代入と setSpeechText を必ずペアで行う唯一の場所。
//     古いポインタを Avatar が見たまま再代入しないよう、この関数以外でバッファに触らない。
static void apiBalloonShow(const String& text) {
    stackChanApiBalloon = text;
    avatar.setSpeechText(stackChanApiBalloon.c_str());
}

// (2) 所有権付き消去: 自分のAPI吹き出しが表示中のときだけ消す。他経路の表示は触らない。
//     タイマーはどちらの場合も無効化する（自分の表示がもう無い以上、待つものがない）。
static void apiBalloonClearIfOwned() {
    if (avatar.getSpeechText() == stackChanApiBalloon.c_str()) {
        avatar.setSpeechText("");   // "" はリテラルなのでバッファ再代入は不要
    }
    balloonClearAtMs = 0;
}

// (3) 満了判定: millis() オーバーフロー対策の符号付き差分比較。
static bool apiBalloonTimerExpired() {
    return balloonClearAtMs != 0 && (int32_t)(millis() - balloonClearAtMs) >= 0;
}
```

- loop() の再生箇所（`main.cpp:517-522`）:
  ```cpp
  if (stackChanApiClient != nullptr && !mod->isBusy()) {
      StackChanApiMessage msg;
      if (stackChanApiClient->takePending(msg)) {
          String incoming = truncateUtf8(msg.balloonText, 10);   // ローカルに確定（永続バッファは未変更）
          bool hasBalloon = (incoming.length() > 0);
          if (hasBalloon) {
              apiBalloonShow(incoming);          // 再生前に表示（音声と同期）
          } else {
              // このメッセージは「吹き出しなし」。自分の旧吹き出しが残っていれば片付けてから再生する
              // （新しい音声と無関係な旧テキストを並走させない）。他経路の表示は消さない。
              apiBalloonClearIfOwned();
          }
          playWavHttp(msg.audioUrl, msg.expression);   // 表情設定＋再生（ブロッキング）
          if (hasBalloon) {
              balloonClearAtMs = millis() + 10000;     // 再生終了後10秒
          }
      }
  }
  if (apiBalloonTimerExpired()) {
      apiBalloonClearIfOwned();
  }
  ```
- 帰結する不変条件（すべての Finding をこの3つで満たす）:
  - `setSpeechText` に渡すポインタは常に `apiBalloonShow` 内で再代入直後の `c_str()` → 表示中バッファの破壊なし。
  - 消去は常に `apiBalloonClearIfOwned` 経由 → 他経路の表示を消す経路が存在しない。
  - 空/欠落 balloon は「吹き出しなし」として自分の旧表示だけ片付ける → 「欠落時は吹き出しなしで再生」の
    互換仕様と一致し、旧サーバー相手でも新しい音声に旧テキストが並走しない。
  - 次の非空メッセージは `apiBalloonShow` が上書き＋タイマー再設定 → 「次メッセージで上書き」も自然に成立。

## 設計上の注意点

- **JSONバッファ容量指定は無視される**。ArduinoJson v7.4.3（`platformio.ini:72` `@ ^7`）は `JsonDocument` が
  伸縮式で、`DynamicJsonDocument(2048)` の容量引数は実質無視され必要なだけ確保する（`20260716-stackchan-api-cursor.md`
  参照）。**ただし伸縮式ゆえに巨大レスポンスはそのぶんヒープを確保してしまう**ので、容量指定に頼らず上記の
  Filter必須化 + 受信バイト数上限（`getSize()<0` や上限超は破棄、または bounded read）+ `balloon` バイト長上限で
  入力サイズ側を制約する（Finding対応）。Content-Length は欠落し得るため「不明なら破棄」で安全側に倒すのが要点。
- **吹き出し状態への操作は3関数（show / clearIfOwned / timerExpired）以外から行わない**。所有権チェックは
  ポインタ同一性に依存しており、バッファの再代入が `apiBalloonShow` の外（一時格納の使い回し等）に漏れると
  Avatar が指す表示中バッファを壊す/dangling 化する。消去が `apiBalloonClearIfOwned` の外に漏れると他経路の
  表示を消す。将来この機能を拡張するときもこの3関数を経由すること。他経路の `setSpeechText` はコピーせず
  ポインタ保持なので、別ポインタ＝上書き済みと確実に判定できる。
- **表情と吹き出しの寿命は独立**。再生終了で顔は Neutral に戻るが、吹き出しは10秒残る（顔は素・吹き出しだけ残る）。
  ユーザー了承済み。
- **吹き出し表示幅**: 全角10文字（`efontJA_16`×`TEXT_SIZE=2`=全角32px/字）で約320px、画面幅ぎりぎり。
  文字は読めるが吹き出しの楕円背景（`Balloon.h:44-48`、文字幅の約2倍径）は画面右へはみ出す。ライブラリ既存仕様で、
  修正するなら Balloon 描画の別改修（今回スコープ外）。
- **AtomS3R**: 128×128 画面に 320×240 スプライトを `setScale(0.5)`＋`setPosition`（`main.cpp:460-461`）で表示。
  固定座標の吹き出しの見え方は実機確認が必要。機能自体は動く。
- **後方互換**: `expression`/`balloon` 欠落時は Happy・吹き出しなしで再生。旧サーバー×新FW でも安全。

## 確認方法

FW は API 未対応でも先行検証できる（フィールド欠落＝フォールバック）。

1. ビルド: `pio run -e m5stack-core2-realtime` / `-cores3-realtime` / `-atoms3r-realtime` が通ること。
2. ダミー応答での実機確認（`expression`/`balloon` を含む JSON を返す簡易サーバー、または手元の StackChan-API に
   カラム追加して `baseUrl` を向ける）:
   (a) 指定表情で再生 (b) 吹き出しに指定文字が表示 (c) 再生後に顔が Neutral 復帰
   (d) 吹き出しが再生終了10秒後に消える (e) 11文字以上が10文字に切れる (f) 日本語が化けない
   (g) `expression`/`balloon` 欠落時に Happy・吹き出しなしで再生。
   (h) **所有権**: 再生後10秒以内にボタン操作/Mod切替/会話で別テキストを表示し、その表示が消されないこと
   （タイマー満了時・空balloonメッセージ受信時のどちらでも）。
   (i) **空balloon**: balloon付きメッセージの10秒窓内に `balloon` 欠落/空の次メッセージを再生 → 旧API吹き出しが
   再生前に消え、新しい音声と旧テキストが並走しないこと。他経路が表示中のテキストは消えないこと。
   (j) **過大入力**: 上限超のレスポンス（巨大 `text`/`balloon`、Content-Length 無し）を返しても破棄され、
   リセットしないこと。
3. API 実装後の結合確認: `POST /api/messages` に `expression`/`balloon` 付きで投入し、`/next` が両フィールドを
   返すこと、実機で反映されることを確認。
