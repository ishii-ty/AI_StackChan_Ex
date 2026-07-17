#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "StackChanApiClient.h"

StackChanApiClient::StackChanApiClient(stackchan_api_s config)
    : _config(config)
{
}

void StackChanApiClient::begin()
{
    _pendingMutex = xSemaphoreCreateMutex();
    xTaskCreate(pollTask, "stackChanApiPoll", 6 * 1024, this, 2, NULL);
}

bool StackChanApiClient::takePendingAudioUrl(String& url)
{
    bool hasPending = false;
    xSemaphoreTake(_pendingMutex, portMAX_DELAY);
    if (_hasPendingAudioUrl) {
        url = _pendingAudioUrl;
        _pendingAudioUrl = "";
        _hasPendingAudioUrl = false;
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
        String payload = http.getString();
        DynamicJsonDocument doc(2048);
        DeserializationError err = deserializeJson(doc, payload);
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
                String audioUrl = _config.baseUrl + doc["audio"]["url"].as<String>();
                Serial.printf("StackChanApi: audio message received (id=%u): %s\n",
                              _lastSeenId, audioUrl.c_str());
                xSemaphoreTake(_pendingMutex, portMAX_DELAY);
                _pendingAudioUrl = audioUrl;
                _hasPendingAudioUrl = true;
                xSemaphoreGive(_pendingMutex);
                handled = true;
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
        // 保留中の音声URLがある間は次のGETを行わない。カーソルは受信時に進むため、保留中に
        // 次をGETすると_pendingAudioUrlを上書きし、まだメインタスクが再生していないメッセージを
        // 取りこぼすため。
        bool hasPending;
        xSemaphoreTake(self->_pendingMutex, portMAX_DELAY);
        hasPending = self->_hasPendingAudioUrl;
        xSemaphoreGive(self->_pendingMutex);

        if (WiFi.status() == WL_CONNECTED && !hasPending) {
            self->pollOnce();
        }
        delay(self->_config.pollIntervalMs);
    }
}
