# Realtime Audio Playback Queue (fix choppy playback)

## 目的

Realtime 系（OpenAI Realtime / Gemini Live / GPT-Live）の応答音声が、チャンクの境目ごとにぷつぷつ途切れる問題を直す。

## 原因（2026-09-23 実機ログで確認）

- OpenAI Realtime の出力チャンクは 1 つ 12000 byte（24kHz PCM16 で 250ms）で、9 個前後ずつまとめて届く。サーバーは再生速度より先送りしているため、ネットワーク遅延は原因ではない。
- `RealtimeLLMBase::streamAudioDelta()` は `while (M5.Speaker.isPlaying())` で前のチャンクが鳴り終わるまで待ってから、次の `playRaw()` を呼んでいる。
- 鳴り終わった時点でスピーカーの出力キューは空になり、次のデータが出るまで無音が入る。そのため、チャンクの境目（250ms ごと）に規則的な途切れが入る。
- M5Unified の `playRaw()` は、1 チャンネルに「再生中 + 待ち」の 2 つを積める（`isPlaying(ch)` が 0/1/2 を返す）。ライブラリのコメントでも、実行中に生成するデータは 3 つのバッファを順に使う方法が推奨されている。

## 対象範囲

- `RealtimeLLMBase` の音声再生処理（ストリーミング再生とバッファ管理）。
- 各 Realtime 派生クラスの、再生終了時のバッファクリア処理（2 面前提の直書きを置き換える）。

以下は変更しない。

- 各サービスのイベント処理、録音処理、セッション処理。
- `REALTIME_API_WITH_TTS` 経路（`audioBuf` を使わない）。
- `driver/Audio.cpp`、`driver/WakeWord.cpp` など、Realtime 以外の再生処理。

## 主な変更ファイル

- `src/llm/RealtimeLLMBase.h` / `.cpp`
- `src/llm/ChatGPT/RealtimeChatGPT.cpp`（バッファクリアの置き換えのみ）
- `src/llm/Gemini/GeminiLive.cpp`（同上）
- `src/llm/ChatGPT/GptLive.cpp`（同上）
- `doc/fw_design.md`
- `doc/codex/steering/20260923-realtime-audio-queue.md`

## 実装方針

1. バッファを 3 面にする。
   - `RealtimeLLMBase.h` に `RT_AUDIO_BUF_NUM`（3）と `RT_AUDIO_BUF_SIZE`（100KB、現状と同じ）を定義し、`audioBuf[RT_AUDIO_BUF_NUM]` にする。
   - 3 面の内訳は、再生中・待ち・次のデコード先。
2. 再生チャンネルを固定し、キューに積む方式にする。
   - `RT_AUDIO_PLAY_CHANNEL`（0）に固定して `playRaw(..., channel, stop_current_sound=false)` を呼ぶ。
   - `streamAudioDelta()` では、デコードの前に `while (M5.Speaker.isPlaying(ch) >= 2)` で待ちが空くのを待つ。3 面を順番に使うため、キューが空いた時点で、次に書き込む面は再生にも待ちにも使われていない。鳴り終わるまでは待たない。
3. デコード後のサイズが `RT_AUDIO_BUF_SIZE` を超える delta は、バッファ溢れを防ぐため、ログを出して捨てる（現状は未チェック）。
4. `clearAudioBuf()` を `RealtimeLLMBase` に追加する。各派生クラスの `for(int i=0; i<2; i++) memset(audioBuf[i], 0, 100 * 1024);` をこれに置き換える。`nextBufIdx` もここで 0 に戻す。
5. リップシンク用の `getAudioLevel()` は、今鳴っている面を参照するように直す。待ちがあれば 2 つ前、なければ 1 つ前に積んだ面。
6. 再生終了の判定（各クラスの `while (M5.Speaker.isPlaying())`）は、全チャンネルの再生終了を待つ今の処理のままでよい。

## 設計上の注意点

- 共通処理の変更なので、OpenAI Realtime・Gemini Live・GPT-Live のすべてに影響する。イベント処理は変えず、再生処理だけを変える。
- メモリは 100KB × 1 面増える（malloc。PSRAM 搭載機では PSRAM から確保される想定）。3 環境とも PSRAM 搭載機。
- 固定チャンネル 0 を Realtime 再生で使う。操作音（`sw_tone()` 等）は自動でチャンネルを選ぶため、衝突しない。
- `REALTIME_API_WITH_TTS` ビルドでは `audioBuf` を確保しないので、`clearAudioBuf()` もその条件で何もしないようにする。

## 確認方法

- ビルド: `m5stack-core2-realtime`、`m5stack-cores3-realtime`、`m5stack-atoms3r-realtime`。
- 実機:
  - OpenAI Realtime（type 0）で長めの応答を再生し、250ms ごとの途切れが無くなること。
  - Gemini Live（type 3）、GPT-Live（type 5）でも途切れずに再生されること。
  - 応答終了後に録音が再開すること（再生終了判定の回帰がないこと）。
  - リップシンクで、発話に合わせて口が動くこと。

## 戻し方

`streamAudioDelta()` の再生を「鳴り終わりを待って `playRaw()`」に戻し、`RT_AUDIO_BUF_NUM` を 2 にする。`clearAudioBuf()` はそのまま使える。
