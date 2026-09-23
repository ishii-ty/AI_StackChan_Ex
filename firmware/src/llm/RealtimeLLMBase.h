#if defined(REALTIME_API)

#ifndef _REALTIME_LLM_BASE_H
#define _REALTIME_LLM_BASE_H

#include <Arduino.h>
#include <M5Unified.h>
#include "StackchanExConfig.h"
#include "SpiRamJsonDocument.h"
#include "ChatHistory.h"
#include "LLMBase.h"
#include <WebSocketsClient.h>

//#define REALTIME_API_RECORD_TEST

#define GEMINI_PROMPT_MAX_SIZE   (1024*50)

#define RT_REC_LENGTH       (2000)      //0.125s 

// ストリーミング再生用バッファ。M5.Speakerの1チャンネルは「再生中+待ち」の2つを積めるため、
// 次のデコード先を加えた3面を順番に使う（2面だと鳴り終わりを待つ必要があり、チャンクの境目で音が途切れる）
#define RT_AUDIO_BUF_NUM        (3)
#define RT_AUDIO_BUF_SIZE       (100 * 1024)
#define RT_AUDIO_PLAY_CHANNEL   (0)
#define RT_REC_SAMPLE_RATE  (16000)

#ifdef REALTIME_API_RECORD_TEST
#define REALTIME_RECORD_TIMEOUT     (4 * 1000)      //ms  ※録音テスト再生用バッファのサイズに合わせる
#else
#define REALTIME_RECORD_TIMEOUT     (30 * 1000)      //ms
#endif

extern String InitBuffer;
extern const String json_ChatString;

class RealtimeLLMBase: public LLMBase{
//private:
public:   //本当はprivateにしたいところだがコールバック関数にthisポインタを渡して使うためにpublicとした
    WebSocketsClient webSocket;
    SpiRamJsonDocument msgDoc;

    // for record
    //
    //int16_t* rtRecBuf;
    int rtRecSamplerate;
    int rtRecLength;
    bool realtime_recording;
    bool response_done;
    portTickType startTime;

#ifdef REALTIME_API_RECORD_TEST
    int16_t* recTestBuf;
    int recTestLenMax;
    int recTestLenCnt;
#endif

    // for play
    //
    uint8_t* audioBuf[RT_AUDIO_BUF_NUM];    // Base64をデコードして得た音声データを格納するバッファ
    int nextBufIdx;          // 次回データを格納するバッファの面
    int playSampleRate;      // 出力音声のサンプルレート。サービスごとに異なるため派生クラスで変更する

public:
    RealtimeLLMBase(llm_param_t param);

    virtual void chat(String text, const char *base64_buf = NULL) {};   //dummy
    virtual String& buildInputAudioJson(String& jsonBuf, String& base64) = 0;

    void invokeWebSocketLoopTask(void);
    void suspendWebSocketLoopTask(void);
    void resumeWebSocketLoopTask(void);
    void webSocketProcess();
    int getAudioLevel();
    virtual void startRealtimeRecord();
    virtual void stopRealtimeRecord();
    void resetRealtimeRecordStartTime();
    portTickType checkRealtimeRecordTimeout();
    bool isRealtimeRecording() {return realtime_recording;};

    int base64_decode(const char* input, int size, char* output);
    void hexdump(const void *mem, uint32_t len, uint8_t cols = 16);
    void streamAudioDelta(String& delta);
    void clearAudioBuf();
    void queuePcm(const uint8_t* pcm, int len);

    // for TTS
    //
    String outputText;

    // webSocketProcess()から呼ぶフック。既定は従来動作のまま
    //
    // falseの間はwebSocket.loop()を呼ばない（切断後の自動再接続を止めたい場合に使う）
    virtual bool isWebSocketActive() { return true; };
    // webSocket.loop()の後に毎回呼ばれる
    virtual void onProcess() {};
    // 録音チャンクを送信した後に呼ばれる
    virtual void onRecordChunk(const int16_t* buf, int len) {};
    // trueの間は録音中の"Listening..."表示で吹き出しを上書きしない
    virtual bool isStatusTextLocked() { return false; };
    // 録音も発話もしていない時の吹き出し表示
    virtual const char* idleStatusText() { return "Please touch"; };
    // suspendWebSocketLoopTask()でタスクを止める直前に、呼び出し元のタスクから呼ばれる
    virtual void beforeSuspend() {};

};


#endif  //_REALTIME_LLM_BASE_H

#endif  //REALTIME_API