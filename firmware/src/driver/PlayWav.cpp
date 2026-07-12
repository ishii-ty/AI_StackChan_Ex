#include <Arduino.h>
#include <M5Unified.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioGeneratorWAV.h>
#include "PlayWav.h"
#include "PlayMP3.h"
#include "Avatar.h"
#include "share/Mutex.h"

using namespace m5avatar;

extern Avatar avatar;
extern bool servo_home;

AudioGeneratorWAV *wav;

void wav_init(void)
{
    wav = new AudioGeneratorWAV();
}

// StackChan-API等、LAN内のプレーンHTTPサーバーが返すWAVをダウンロードしながら再生する。
// メインタスク(main.cppのloop())から呼ばれる想定。Realtime系タスクのMic/Speaker操作と
// 直列化するため、playMP3()系と同様にここでenterMutexAudio()/exitMutexAudio()を行う。
bool playWavHttp(const String& url)
{
    bool result = false;

    enterMutexAudio();

    AudioFileSourceHTTPStream *httpStream = new AudioFileSourceHTTPStream(url.c_str());
    if (httpStream->isOpen()) {
        AudioFileSourceBuffer *buff = new AudioFileSourceBuffer(httpStream, preallocateBuffer, preallocateBufferSize);

        avatar.setExpression(Expression::Happy);
        servo_home = false;

        M5.Mic.end();
        M5.Speaker.begin();

        wav->begin(buff, &out);
        Serial.println("wav start");
        while (wav->isRunning()) {
            if (!wav->loop()) {
                wav->stop();
                Serial.println("wav stop");
            }
            delay(1);
        }

        M5.Speaker.end();
        M5.Mic.begin();

        avatar.setExpression(Expression::Neutral);
        servo_home = true;

        delete buff;
        result = true;
    } else {
        Serial.printf("StackChanApi: failed to open audio stream: %s\n", url.c_str());
    }
    delete httpStream;

    exitMutexAudio();

    return result;
}
