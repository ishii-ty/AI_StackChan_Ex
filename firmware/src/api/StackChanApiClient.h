#ifndef _STACKCHAN_API_CLIENT_H
#define _STACKCHAN_API_CLIENT_H

#include <Arduino.h>
#include "StackchanExConfig.h"

// 外部StackChan-APIサーバー(../StackChan-API等)を定期的にポーリングし、
// audio.urlが返された場合はURLを保留として保持する。実際の再生はメインタスク
// (main.cppのloop())が isBusy() でない時に takePendingAudioUrl() で取り出して行う。
// このタスクはネットワークI/O(HTTP GET + JSONパース)のみを行い、音声デバイスには触れない。
//
// サーバーは配信状態を持たない(非破壊)ため、どこまで受け取ったかを示すカーソル(id)は
// 端末側で保持し、次回以降のリクエストに ?after=<id> として載せる責務もこのクラスが持つ。
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

        // 受信済みメッセージのidを保持するカーソル。0は「未受信(初回)」を表す。
        // pollTask(pollOnce())からのみ読み書きするため_pendingMutexによる保護は不要
        // (_pendingMutexが守るのはメインタスクと共有する_pendingAudioUrlのみ)。
        // 電源断で0に戻る(永続化しない)ため、再起動後は直近1時間分を再生し直して追いつく。
        uint32_t _lastSeenId = 0;

        bool pollOnce();  // GET {baseUrl}/api/messages/next[?after=<id>] を1回実行する
        static void pollTask(void *args);
};

#endif
