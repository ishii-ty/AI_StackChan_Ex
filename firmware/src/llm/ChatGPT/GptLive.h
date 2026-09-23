#if defined(REALTIME_API) && !defined(REALTIME_API_WITH_TTS)

#ifndef _GPT_LIVE_H
#define _GPT_LIVE_H

#include <Arduino.h>
#include <M5Unified.h>
#include "StackchanExConfig.h"
#include "SpiRamJsonDocument.h"
#include "../ChatHistory.h"
#include "../RealtimeLLMBase.h"
#include "ChatGPT.h"
#include <WebSocketsClient.h>

// OpenAI GPT-Live (wss://api.openai.com/v1/live/sessions)
//
// 接続時間で課金されるため常時接続はせず、タッチで接続して会話を始め、
// 無操作タイムアウトまたは会話中のタッチでセッションを閉じる。
// Mic と Speaker を同時に使えないボードのため、応答再生中は入力を mute する疑似半二重で動かす。
class GptLive: public RealtimeLLMBase{
public:   //本当はprivateにしたいところだがコールバック関数にthisポインタを渡して使うためにpublicとした
    enum LiveState {
        LIVE_IDLE,          // 未接続
        LIVE_CONNECTING,    // WebSocket接続〜session.started待ち
        LIVE_ACTIVE,        // 会話中
        LIVE_CLOSING,       // session.close送信〜session.closed待ち
    };

    MCPClient* mcpClient[LLM_N_MCP_SERVERS_MAX];
    FunctionCall* fnCall;

    String role;
    String userInfo;
    String systemRole;

    String liveModel;
    String delegationModel;
    String liveVoice;
    String authHeader;

    volatile LiveState state;
    volatile bool openRequested;    // タッチによる会話開始の要求（メインループのタスクから立てる）
    volatile bool closeRequested;   // タッチ・録音タイムアウトによる会話終了の要求
    bool playing;                   // 応答音声の再生中（Mic停止・mutexAudio取得中）
    bool discardingBurst;           // 相槌と判定した出力音声のまとまりを捨てている最中
    bool lastBurstWasBackchannel;   // 直近の出力音声が相槌だった（遅れて届くtranscriptの表示先判定用）
    unsigned long stateStartMs;     // CONNECTING / CLOSING に入った時刻
    unsigned long lastActivityMs;   // 無操作タイマーの基準
    unsigned long lastVoiceMs;      // ローカルで発話を検出した最終時刻
    int lastVoicePeak;              // 発話検出時の録音チャンクのピーク振幅（閾値調整のログ用）
    unsigned long recordResumeMs;   // 録音を開始・再開した時刻（直後の雑音を発話と誤判定しないため）
    unsigned long burstStartMs;     // 相槌と判定した声のまとまりの開始時刻
    unsigned long lastVoicedOutMs;  // 声の入った出力音声を最後に受信した時刻
    uint8_t* pcmScratch;            // 出力音声を振幅判定のために先にデコードする作業バッファ
    unsigned long balloonUntilMs;   // 相槌テキストを表示し続ける期限
    int eventSeq;
    char balloonBuf[128];           // setSpeechText()はポインタを保持するため固定バッファで渡す

public:
    GptLive(llm_param_t param);

    virtual void chat(String text, const char *base64_buf = NULL) {};   //dummy
    virtual String& buildInputAudioJson(String& jsonBuf, String& base64);
    virtual void load_role();

    virtual void startRealtimeRecord();
    virtual void stopRealtimeRecord();
    virtual bool isWebSocketActive() { return state != LIVE_IDLE; };
    virtual void onProcess();
    virtual void onRecordChunk(const int16_t* buf, int len);
    virtual bool isStatusTextLocked() { return (long)(balloonUntilMs - millis()) > 0; };
    virtual const char* idleStatusText();
    virtual void beforeSuspend();

    void openSession();
    void closeSession();
    void releaseConnection();
    void sendSessionStart();
    void beginPlayback();
    void endPlayback(bool resumeInput);
    void handleOutputAudio(String& delta);
    void handleOutputTranscript(const char* delta);
    void handleResponseEvent(JsonVariant event);
    void sendControl(const char* type);
    String nextEventId(const char* prefix);
};


#endif  //_GPT_LIVE_H

#endif  //REALTIME_API && !REALTIME_API_WITH_TTS
