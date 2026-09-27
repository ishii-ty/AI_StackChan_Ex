# Realtime Record Buffer (issue #4)

## 目的

Realtime 系（OpenAI Realtime / Gemini Live / GPT-Live）の録音送信で、次の 2 つを直す（GitHub issue #4）。

1. マイクが書き込み中のバッファを送信している問題（開始直後の古いデータ送信を含む）。
2. OpenAI Realtime で、入力音声のサンプリングレートの宣言（24kHz）と実際の録音（16kHz）が合っていない問題。

## 原因（2026-09-27 コードで確認）

### 1. 書き込み中のバッファを送信している

- `RealtimeLLMBase::webSocketProcess()` は、1 つの静的バッファ `rtRecBuf` に `M5.Mic.record()` を呼び、直後に base64 変換して送信している。
- M5Unified（0.2.15）の `Mic_Class::_rec_raw()` は、録音の予約枠（2 つ）が空くのを待って予約を登録するだけで、録音の完了は待たずに返る。録音は `mic_task` がバックグラウンドで行う。
- 常時録音中は、`record()` が返るのは「前のチャンク N-1 の録音が終わって予約枠が空いた瞬間」で、その時点で `mic_task` は予約済みのチャンク N の録音を同じ `rtRecBuf` の先頭から始めている。
- そのため送信データは「チャンク N-1 の先頭が N で上書きされたもの」になる。`onRecordChunk()`（GPT-Live の発話判定）は `sendTXT()` の後に呼ばれるので、上書きされる量はさらに多い。
- 録音開始直後は予約枠が 2 つとも空いているので、最初の 2 回の `record()` はすぐ返る。そのため最初の約 250ms 分は、前回の録音の残り（または 0）が送られている。
- `M5.Mic.end()` は予約枠を消さない。`begin()` の後、途中まで録音した予約の続きが録音される（GPT-Live の再生切り替えで発生する）。

### 2. OpenAI Realtime のサンプリングレート不一致

- `RealtimeChatGPT.cpp` の `session.update` は、入力音声を `audio/pcm`・`rate: 24000` と宣言している（OpenAI Realtime の `audio/pcm` は 24kHz のみ対応）。
- 実際の録音は `RT_REC_SAMPLE_RATE`（16kHz）なので、サーバー側では 1.5 倍速・高いピッチの音声として扱われている。
- GPT-Live は入出力とも 16kHz で揃っている。Gemini Live は `audio/pcm`（レート省略時は 16kHz）なので、どちらも問題ない。

## 対象範囲

- `RealtimeLLMBase` の録音・送信処理（`webSocketProcess()`、録音バッファの管理、録音開始処理）。
- `RealtimeChatGPT` の録音サンプリングレートと 1 チャンクの長さ。

以下は変更しない。

- 各サービスのイベント処理、セッション処理、再生処理。
- GPT-Live の発話判定のロジック（`onRecordChunk()` の中身）と各種しきい値。
- Gemini Live・GPT-Live の録音レート（16kHz のまま）。
- `driver/` 配下の録音処理（WakeWord、Whisper 等）。

## 主な変更ファイル

- `src/llm/RealtimeLLMBase.h` / `.cpp`
- `src/llm/ChatGPT/RealtimeChatGPT.cpp`（コンストラクタで録音レート・長さを設定）
- `doc/fw_design.md`
- `doc/codex/steering/20260927-realtime-rec-buffer.md`

## 実装方針

コミットは 2 つに分ける（1 → 2 の順）。

### コミット 1: 録音バッファを 3 面にする

1. 録音バッファを 3 面のリングバッファにする。
   - `RealtimeLLMBase.h` に `RT_REC_BUF_NUM`（3）と `RT_REC_LENGTH_MAX`（1 面の最大サンプル数）を定義する。
   - 静的配列 `rtRecBuf` をやめ、コンストラクタで `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` で 3 面を確保する。Core2 は内部ヒープに余裕がないため PSRAM に置く。`mic_task` は I2S の DMA バッファから CPU でコピーしているので、書き込み先が PSRAM でも問題ない。
2. 送信する面を「2 つ前に予約した面」にする。
   - `record(buf[k])` が返った時点で、予約枠は 2 つなので `buf[k-2]` の録音は完了している（`buf[k-1]` は録音中、`buf[k]` は予約済み）。
   - `buf[k-2]` に次に予約するのは次のループの `record()` なので、送信中・`onRecordChunk()` 中に上書きされることはない。
   - `isRecording()` を監視する必要はない。送るのは完了したチャンクのうち最新のものなので、遅延も増えない。
   - base64 変換、`REALTIME_API_RECORD_TEST` の蓄積、`onRecordChunk()` はすべてこの完了した面を使う。
3. 録音開始直後の 2 チャンクは送らない。
   - `startRealtimeRecord()` で「開始後に録ったチャンク数」を 0 に戻し、2 以上になってから送信する。
   - 面の番号（`k`）はリセットしない。停止時に残った予約が再開後に完了しても、面と予約の対応が崩れないようにするため。
4. ループの周期は変えない（`record()` の待ちで約 125ms ごと）。

### コミット 2: OpenAI Realtime を 24kHz で録音する

1. `RealtimeChatGPT` のコンストラクタで `rtRecSamplerate = 24000`、`rtRecLength = 3000`（125ms）にする。
2. `RT_REC_LENGTH_MAX` は 3000 にする（3 面 × 3000 サンプル × 2 byte = 18KB、PSRAM）。
3. `session.update` の宣言（24kHz）は変えない。

## 設計上の注意点

- 共通処理の変更なので、OpenAI Realtime・Gemini Live・GPT-Live のすべてに影響する。
- 送信の開始が約 250ms 遅れる（開始直後の 2 チャンクを捨てるため）。現状も開始直後の 2 チャンクは古いデータなので、実質の遅れはない。
- GPT-Live の `GPT_LIVE_VAD_GUARD_MS`（300ms）は、録音再開直後の雑音を避けるためのもの。最初の `onRecordChunk()` が約 250ms 遅れて呼ばれるが、判定は `recordResumeMs` からの経過時間で行っているので、動作は変わらない。
- 録音のタイムアウト処理（`checkRealtimeRecordTimeout()`）は経過時間で判定しているので影響しない。
- 24kHz 録音の送信量は 1.5 倍（base64 で約 5.3KB → 8KB / 125ms）。OpenAI Realtime のみで、帯域上の問題はない想定。
- `REALTIME_API_RECORD_TEST` のテスト用バッファは `rtRecLength` から計算しているので、24kHz でもそのまま使える。
- `REALTIME_API_WITH_TTS` 経路も同じ録音処理を使うので、同様に直る。

## 確認方法

- ビルド: `m5stack-cores3-realtime`、`m5stack-core2-realtime`、`m5stack-atoms3r-realtime`。
- 録音テスト（`REALTIME_API_RECORD_TEST` を一時的に有効にする）:
  - 録音を再生し、125ms ごとのつなぎ目のノイズが無いこと。
  - OpenAI Realtime（type 0）で、再生音のピッチ・速さが正常であること。
- 実機:
  - OpenAI Realtime（type 0）、Gemini Live（type 3）、GPT-Live（type 5）で会話ができること。
  - OpenAI Realtime で、音声認識（文字起こし）の精度が悪化していないこと。
  - GPT-Live で、応答の再生後に録音が再開し、相槌判定が動くこと。
  - Core2 で、起動・会話中にヒープ不足が起きないこと。

## 戻し方

- コミット 2: `RealtimeChatGPT` のコンストラクタでの設定を消す（16kHz に戻る）。
- コミット 1: `webSocketProcess()` を「1 面に録音して直後に送信」に戻す。
