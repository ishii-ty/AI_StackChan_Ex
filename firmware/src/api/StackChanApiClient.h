#ifndef _STACKCHAN_API_CLIENT_H
#define _STACKCHAN_API_CLIENT_H

#include <Arduino.h>
#include "StackchanExConfig.h"

// 外部StackChan-APIサーバー(../StackChan-API等)を定期的にポーリングし、
// audio.urlが返された場合はURLを保留として保持する。実際の再生はメインタスク
// (main.cppのloop())が isBusy() でない時に takePendingAudioUrl() で取り出して行う。
// このタスクはネットワークI/O(HTTP GET + JSONパース)のみを行い、音声デバイスには触れない。
class StackChanApiClient {
    public:
        StackChanApiClient(stackchan_api_s config);
        void begin();  // ポーリング用FreeRTOSタスクを起動する

        // 保留中の音声URLがあればurlにコピーして保留をクリアし、trueを返す。
        // メインタスク(loop())から呼ぶことを想定する。
        bool takePendingAudioUrl(String& url);

    private:
        stackchan_api_s _config;

        SemaphoreHandle_t _pendingMutex = nullptr;
        String _pendingAudioUrl;
        bool _hasPendingAudioUrl = false;

        bool pollOnce();  // GET {baseUrl}/api/messages/next を1回実行する
        static void pollTask(void *args);
};

#endif
