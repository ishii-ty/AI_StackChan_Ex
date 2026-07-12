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
        } else if (doc["audio"]["url"].is<const char*>()) {
            String audioUrl = _config.baseUrl + doc["audio"]["url"].as<String>();
            Serial.printf("StackChanApi: audio message received: %s\n", audioUrl.c_str());
            xSemaphoreTake(_pendingMutex, portMAX_DELAY);
            _pendingAudioUrl = audioUrl;
            _hasPendingAudioUrl = true;
            xSemaphoreGive(_pendingMutex);
            handled = true;
        }
    } else if (httpCode == HTTP_CODE_NO_CONTENT) {
        // 未配信メッセージなし。何もしない。
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
        // 保留中の音声URLがある間は次のGETを行わない。保留中に次をGETするとサーバー側で
        // delivered扱いになり、まだメインタスクが再生していないメッセージを取りこぼすため。
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
