#ifndef _STACKCHAN_API_CLIENT_H
#define _STACKCHAN_API_CLIENT_H

#include <Arduino.h>
#include "StackchanExConfig.h"

// 外部StackChan-APIサーバー(../StackChan-API等)を定期的にポーリングし、
// audio.urlが返された場合はexpression/balloonとあわせて保留として保持する。実際の再生はメインタスク
// (main.cppのloop())が isBusy() でない時に takePending() で取り出して行う。
// このタスクはネットワークI/O(HTTP GET + JSONパース)のみを行い、音声デバイスには触れない。
//
// サーバーは配信状態を持たない(非破壊)ため、どこまで受け取ったかを示すカーソル(id)は
// 端末側で保持し、次回以降のリクエストに ?after=<id> として載せる責務もこのクラスが持つ。

// ポーリングで受信した1件分の保留メッセージ。
struct StackChanApiMessage {
    String audioUrl;
    String balloonText;
    String expression;
};

class StackChanApiClient {
    public:
        StackChanApiClient(stackchan_api_s config);
        void begin();  // ポーリング用FreeRTOSタスクを起動する

        // 保留中のメッセージがあればmsgにコピーして保留をクリアし、trueを返す。
        // メインタスク(loop())から呼ぶことを想定する。
        bool takePending(StackChanApiMessage& msg);

    private:
        stackchan_api_s _config;

        SemaphoreHandle_t _pendingMutex = nullptr;
        StackChanApiMessage _pending;
        bool _hasPending = false;

        // 受信済みメッセージのidを保持するカーソル。0は「未受信(初回)」を表す。
        // pollTask(pollOnce())からのみ読み書きするため_pendingMutexによる保護は不要
        // (_pendingMutexが守るのはメインタスクと共有する_pendingのみ)。
        // 電源断で0に戻る(永続化しない)ため、再起動後は直近1時間分を再生し直して追いつく。
        uint32_t _lastSeenId = 0;

        bool pollOnce();  // GET {baseUrl}/api/messages/next[?after=<id>] を1回実行する
        static void pollTask(void *args);
};

#endif
