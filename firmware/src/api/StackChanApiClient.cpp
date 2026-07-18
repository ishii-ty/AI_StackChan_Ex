#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "StackChanApiClient.h"

// レスポンス本文の受信サイズ上限。ArduinoJson v7のJsonDocumentは伸縮式でDynamicJsonDocumentの
// 容量指定が実質無視されるため、getString()前にHTTPレスポンスの実バイト数でヒープ先食いを防ぐ。
static const int MAX_RESPONSE_BYTES = 4096;
// balloonフィールドのバイト長上限(全角約21文字ぶん)。表示は10文字に切り詰めるが、
// 受信段階でも過大な入力を弾く。
static const size_t MAX_BALLOON_BYTES = 64;

StackChanApiClient::StackChanApiClient(stackchan_api_s config)
    : _config(config)
{
}

void StackChanApiClient::begin()
{
    _pendingMutex = xSemaphoreCreateMutex();
    xTaskCreate(pollTask, "stackChanApiPoll", 6 * 1024, this, 2, NULL);
}

bool StackChanApiClient::takePending(StackChanApiMessage& msg)
{
    bool hasPending = false;
    xSemaphoreTake(_pendingMutex, portMAX_DELAY);
    if (_hasPending) {
        msg = _pending;
        _pending = StackChanApiMessage();
        _hasPending = false;
        hasPending = true;
    }
    xSemaphoreGive(_pendingMutex);
    return hasPending;
}

bool StackChanApiClient::pollOnce()
{
    bool handled = false;
    String url = _config.baseUrl + "/api/messages/next";
    // サーバーは配信状態を持たないため、afterを送らないと同じ1件を返し続ける。
    // 初回(_lastSeenId==0)のみパラメータ無しでリクエストする。
    if (_lastSeenId > 0) {
        url += "?after=" + String(_lastSeenId);
    }

    HTTPClient http;
    if (!http.begin(url)) {
        Serial.printf("StackChanApi: failed to begin HTTP request: %s\n", url.c_str());
        return false;
    }

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        // getString()前に実バイト数で上限を掛ける。http.getSize()はContent-Lengthが無い/chunkedだと
        // 負値(長さ不明)を返し、これを許すとgetString()が巨大本文を丸ごとStringに確保してヒープを
        // 先食いしリセットしうるため、不明/上限超は破棄する(安全側に倒す)。
        int len = http.getSize();
        if (len < 0 || len > MAX_RESPONSE_BYTES) {
            Serial.printf("StackChanApi: response size rejected (size=%d)\n", len);
        } else {
            String payload = http.getString();

            // ArduinoJson v7のJsonDocumentは伸縮式でDynamicJsonDocumentの容量指定が実質無視されるため、
            // 未使用のtext(最大500文字)等を読み込まないようFilterで必要なフィールドのみ抽出する。
            JsonDocument filter;
            filter["id"] = true;
            filter["expression"] = true;
            filter["balloon"] = true;
            filter["audio"] = true;

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
            if (err) {
                Serial.printf("StackChanApi: JSON parse failed: %s\n", err.c_str());
            } else if (!doc["id"].is<uint32_t>()) {
                // idが無いとカーソルを進められない。ここで再生だけ行うと次回も同じメッセージが
                // 返り、同じ音声を再生し続けることになるため再生もしない。
                Serial.println("StackChanApi: response has no id, skipped");
            } else {
                // 再生の成否に関わらず受信時点でカーソルを進める。再生できないメッセージが
                // あっても先へ進めるようにするため(同じメッセージでの無限リトライを避ける)。
                _lastSeenId = doc["id"].as<uint32_t>();

                // audioが無い(textのみの)メッセージは再生しない。カーソルだけ進めることで
                // 後続の音声メッセージがブロックされないようにする。
                if (doc["audio"]["url"].is<const char*>()) {
                    StackChanApiMessage msg;
                    msg.audioUrl = _config.baseUrl + doc["audio"]["url"].as<String>();
                    msg.expression = doc["expression"] | "";

                    // balloonはバイト長上限を超えたら破棄(空扱い)する。表示側の10文字切り詰めとは別の、
                    // 受信段階での過大入力対策。
                    String balloon = doc["balloon"] | "";
                    if (balloon.length() > MAX_BALLOON_BYTES) {
                        Serial.printf("StackChanApi: balloon too long (%u bytes), discarded\n", balloon.length());
                    } else {
                        msg.balloonText = balloon;
                    }

                    Serial.printf("StackChanApi: audio message received (id=%u): %s\n",
                                  _lastSeenId, msg.audioUrl.c_str());
                    xSemaphoreTake(_pendingMutex, portMAX_DELAY);
                    _pending = msg;
                    _hasPending = true;
                    xSemaphoreGive(_pendingMutex);
                    handled = true;
                }
            }
        }
    } else if (httpCode == HTTP_CODE_NO_CONTENT) {
        // 該当メッセージなし。何もしない。
    } else if (httpCode > 0) {
        Serial.printf("StackChanApi: unexpected HTTP status: %d\n", httpCode);
    } else {
        Serial.printf("StackChanApi: HTTP GET failed: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
    return handled;
}

void StackChanApiClient::pollTask(void *args)
{
    StackChanApiClient *self = (StackChanApiClient *)args;
    for (;;) {
        // 保留中のメッセージがある間は次のGETを行わない。カーソルは受信時に進むため、保留中に
        // 次をGETすると_pendingを上書きし、まだメインタスクが再生していないメッセージを
        // 取りこぼすため。
        bool hasPending;
        xSemaphoreTake(self->_pendingMutex, portMAX_DELAY);
        hasPending = self->_hasPending;
        xSemaphoreGive(self->_pendingMutex);

        if (WiFi.status() == WL_CONNECTED && !hasPending) {
            self->pollOnce();
        }
        delay(self->_config.pollIntervalMs);
    }
}
