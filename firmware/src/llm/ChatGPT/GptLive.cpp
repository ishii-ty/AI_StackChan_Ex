#if defined(REALTIME_API) && !defined(REALTIME_API_WITH_TTS)

#include <Arduino.h>
#include <M5Unified.h>
#include <Avatar.h>
#include "share/Mutex.h"
#include <WiFiClientSecure.h>
#include "rootCA/rootCACertificate.h"
#include <ArduinoJson.h>
#include "SpiRamJsonDocument.h"
#include "GptLive.h"
#include "FunctionCall.h"
#include "MCPClient.h"
#include "Robot.h"

#include <WebSocketsClient.h>

using namespace m5avatar;
extern Avatar avatar;

#define GPT_LIVE_DEFAULT_MODEL              "gpt-live-1"
#define GPT_LIVE_DEFAULT_DELEGATION_MODEL   "gpt-5.6-luna"
#define GPT_LIVE_DEFAULT_VOICE              "marin"

#define GPT_LIVE_SAMPLE_RATE        (16000)         // 入出力共通。マイク録音(RT_REC_SAMPLE_RATE)とそろえる
#define GPT_LIVE_IDLE_TIMEOUT_MS    (30 * 1000)     // 無操作でセッションを閉じるまでの時間
#define GPT_LIVE_CONNECT_TIMEOUT_MS (15 * 1000)     // 接続〜session.startedの待ち時間
#define GPT_LIVE_CLOSE_TIMEOUT_MS   (3 * 1000)      // session.close〜session.closedの待ち時間
#define GPT_LIVE_OUTPUT_END_MS      (600)           // 声の入った出力音声がこの時間途切れたら応答の終わりとみなす
#define GPT_LIVE_OUT_VOICE_LEVEL    (300)           // 出力チャンクのピーク振幅がこれを超えたら声が入っているとみなす（実機で調整）
#define GPT_LIVE_VAD_LEVEL          (1500)          // 録音チャンクのピーク振幅がこれを超えたら発話中とみなす（実機で調整）
#define GPT_LIVE_VAD_HOLD_MS        (500)           // 発話検出後、この時間は発話中として扱う
#define GPT_LIVE_VAD_GUARD_MS       (300)           // 録音の開始・再開直後はMic起動時の雑音やスピーカーの残響を拾うため発話判定しない
#define GPT_LIVE_BACKCHANNEL_MAX_MS (1500)          // 相槌として捨てる声のまとまりの上限。これより長く続けば応答とみなして再生する
#define GPT_LIVE_BALLOON_HOLD_MS    (2000)          // 相槌テキストの表示時間
#define GPT_LIVE_SUSPEND_WAIT_MS    (5 * 1000)      // Mod切替でタスクを止める前に、セッションを閉じ終わるのを待つ上限
#define GPT_LIVE_WS_DRAIN_MAX       (20)            // 1周で追加処理する受信フレームの上限
#define GPT_LIVE_WS_DRAIN_BUDGET_MS (40)            // 1周で追加処理に使う時間の上限（録音チャンク125msより十分短く）

// WebSocketのコールバック関数としてクラスメソッドを渡せないので、コールバック関数を
// 通常の関数にして静的変数を経由してクラスのthisポインタを渡す。
static GptLive* p_this;

static void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    String msgType, delta;
    DeserializationError error;

    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[Live] Disconnected!\n");
            if(p_this->state == GptLive::LIVE_CONNECTING){
                // GPT-Liveはハンドシェイク時にAPIキーとヘッダを検査し、NGならHTTP 401/403で接続を拒否する
                Serial.println("[Live] handshake rejected. Check the API key and its access to GPT-Live.");
            }
            if(p_this->state != GptLive::LIVE_IDLE && p_this->state != GptLive::LIVE_CONNECTING){
                // 接続中の失敗はライブラリが再試行するので、接続タイムアウトで打ち切る
                p_this->releaseConnection();
            }
            break;
        case WStype_CONNECTED:
            Serial.printf("[Live] Connected to url: %s\n", payload);
            p_this->sendSessionStart();
            break;
        case WStype_TEXT:
            error = deserializeJson(p_this->msgDoc, payload);
            if (error) {
                Serial.printf("[Live] JSON deserialization error %d\n", error.code());
                break;
            }

            msgType = p_this->msgDoc["type"].as<String>();
            if(msgType.equals("response.event")){
                // 委譲先モデルのイベント。中身の型を出して関数呼び出しの有無を確認できるようにする
                Serial.printf("[Live] event: response.event (%s)\n",
                              (const char*)(p_this->msgDoc["event"]["type"] | ""));
            }
            else if(!msgType.equals("session.output_audio.delta")){
                Serial.printf("[Live] event: %s\n", msgType.c_str());
            }

            if(msgType.equals("session.started")){
                Serial.printf("[Live] payload: %s\n", payload);
                p_this->state = GptLive::LIVE_ACTIVE;
                p_this->lastActivityMs = millis();
                p_this->recordResumeMs = millis();
                p_this->RealtimeLLMBase::startRealtimeRecord();
            }
            else if(msgType.equals("session.output_audio.delta")){
                delta = p_this->msgDoc["delta"].as<String>();
                p_this->handleOutputAudio(delta);
            }
            else if(msgType.equals("session.output_transcript.delta")){
                p_this->handleOutputTranscript(p_this->msgDoc["delta"] | "");
            }
            else if(msgType.equals("session.input_transcript.delta")){
                Serial.printf("[Live] input: %s\n", (const char*)(p_this->msgDoc["delta"] | ""));
                p_this->lastActivityMs = millis();
                p_this->resetRealtimeRecordStartTime();
            }
            else if(msgType.equals("response.event")){
                p_this->handleResponseEvent(p_this->msgDoc["event"]);
            }
            else if(msgType.equals("session.closed")){
                Serial.printf("[Live] session closed: %s\n", payload);
                p_this->releaseConnection();
            }
            else if(msgType.equals("error")){
                Serial.printf("[Live] payload: %s\n", payload);
            }
            break;
        case WStype_BIN:
            Serial.printf("[Live] get binary length: %u\n", length);
            break;
        case WStype_ERROR:
        case WStype_FRAGMENT_TEXT_START:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT:
        case WStype_FRAGMENT_FIN:
            Serial.printf("[Live] payload: %s\n", payload);
            break;
        default:
            Serial.printf("[Live] Unknown event\n");
            break;
    }
}


// GPT-Liveの音声モデルはツールを持たず、委譲した時だけ委譲先モデルがツールを実行する。
// 委譲すべき場面を会話側のinstructionsで伝えないと、「表情を変えて」等に口で返事をするだけになる。
static const char live_delegation_instructions[] =
    "You can control Stack-chan's body and check information only by delegating to the backend. "
    "Always delegate when the user asks you to change your facial expression, set or change a timer or alarm, "
    "change the speaker volume or screen brightness, check the date, time or day of the week, "
    "manage the wake word, or do anything else that needs a tool. "
    "Do not just say that you did it; delegate so that it is actually done.";

static const char live_delegation_memory_instructions[] =
    " Also delegate when the user tells you something worth remembering about themselves, "
    "so that it is saved to long-term memory.";

static String valueOrDefault(const String& value, const char* defaultValue)
{
    if(value.length() == 0 || value == "null"){
        return String(defaultValue);
    }
    return value;
}

GptLive::GptLive(llm_param_t param)
  : RealtimeLLMBase(param),
    role(""),
    userInfo("User Info: "),
    systemRole(""),
    state(LIVE_IDLE),
    openRequested(false),
    closeRequested(false),
    playing(false),
    discardingBurst(false),
    lastBurstWasBackchannel(false),
    stateStartMs(0),
    lastActivityMs(0),
    lastVoiceMs(0),
    lastVoicePeak(0),
    recordResumeMs(0),
    burstStartMs(0),
    lastVoicedOutMs(0),
    pcmScratch(NULL),
    balloonUntilMs(0),
    eventSeq(0)
{
  p_this = this;    //コールバック関数に静的変数経由でthisポインタを渡す
  msgDoc = SpiRamJsonDocument(1024*150);
  playSampleRate = GPT_LIVE_SAMPLE_RATE;
  pcmScratch = (uint8_t*)heap_caps_malloc(RT_AUDIO_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  balloonBuf[0] = '\0';

  liveModel = valueOrDefault(param.llm_conf.liveModel, GPT_LIVE_DEFAULT_MODEL);
  delegationModel = valueOrDefault(param.llm_conf.delegationModel, GPT_LIVE_DEFAULT_DELEGATION_MODEL);
  liveVoice = valueOrDefault(param.llm_conf.liveVoice, GPT_LIVE_DEFAULT_VOICE);
  Serial.printf("[Live] model: %s, delegation: %s, voice: %s\n",
                liveModel.c_str(), delegationModel.c_str(), liveVoice.c_str());

  initMcpClientList(mcpClient, param.llm_conf.mcpServer, param.llm_conf.nMcpServers);
  fnCall = new FunctionCall(param, this, mcpClient);

  enableMemory(param.llm_conf.enableMemory);
  if(enableMemory()){
    Serial.println("Memory is enabled");
    M5.Lcd.println("Memory is enabled");
  }
  load_role();

  // 接続はタッチで会話を始めるまで行わない（接続時間で課金されるため）
  webSocket.onEvent(webSocketEvent);
  authHeader = "Bearer " + param.api_key;
  // ライブラリ既定の Origin: file:// と Sec-WebSocket-Protocol: arduino を付けると、
  // GPT-LiveはHTTP 403で接続を拒否する（Realtime APIは受け付ける）。どちらが原因かは未切り分けのため両方外す
  // Originはbegin()でリセットされないのでここで一度だけ外す
  webSocket.setExtraHeaders("");
  webSocket.setReconnectInterval(5000);
}


void GptLive::load_role(){
  if(enableMemory()){
    systemRole = systemRole_memory;
  }else{
    systemRole = systemRole_noMemory;
  }
  systemRole += " " + systemRole_realtimeAvatarExpression;

  if(load_system_prompt_from_spiffs()){
    role = String((const char*)systemPrompt["messages"][SYSTEM_PROMPT_INDEX_USER_ROLE]["content"]);
    if (role == "") {
      Serial.println("SPIFFS user role is empty. set default role.");
      role = defaultRole;
    }

    userInfo = String((const char*)systemPrompt["messages"][SYSTEM_PROMPT_INDEX_USER_INFO]["content"]);
    int idx = userInfo.indexOf("User Info");
    if(idx < 0 || !enableMemory()){
      userInfo = "User Info: ";
    }
  }else{
    role = defaultRole;
    userInfo = "User Info: ";
  }
}

String& GptLive::buildInputAudioJson(String& jsonBuf, String& base64)
{
    jsonBuf.concat("{\"type\":\"session.input_audio.append\",\"audio\":\"");
    jsonBuf.concat(base64);
    jsonBuf.concat("\"}");
    return jsonBuf;
}

String GptLive::nextEventId(const char* prefix)
{
    return String(prefix) + "_" + String(++eventSeq);
}

void GptLive::sendControl(const char* type)
{
    JsonDocument doc;
    doc["type"] = type;
    doc["event_id"] = nextEventId("ctl");
    String json;
    serializeJson(doc, json);
    webSocket.sendTXT(json);
}


// タッチ：未接続なら会話を始め、会話中なら終える
// タッチはメインループのタスクから呼ばれるため、ここでは要求を記録するだけにする。
// 接続・切断（SSL送信やMic/Speaker操作）は webSocketLoopTask の onProcess() で行い、
// 同じSSL接続への同時書き込みを避ける。
void GptLive::startRealtimeRecord()
{
    if(state == LIVE_IDLE){
        openRequested = true;
    }else{
        closeRequested = true;
    }
}

// 会話中のタッチ（録音中）と録音タイムアウトで呼ばれる。
// 再生のために録音を一時停止する場合は RealtimeLLMBase::stopRealtimeRecord() を直接呼ぶ。
void GptLive::stopRealtimeRecord()
{
    closeRequested = true;
}

void GptLive::openSession()
{
    Serial.println("[Live] open session");
    load_role();    // update_memoryの結果を次の会話に反映する
    avatar.setSpeechText("Connecting...");
    state = LIVE_CONNECTING;
    stateStartMs = millis();
    // ライブラリ既定の Sec-WebSocket-Protocol: arduino は送らない（コンストラクタの Origin を参照）
    webSocket.beginSslWithCA("api.openai.com", 443, "/v1/live/sessions", root_ca_openai, "");
    // begin()は認証ヘッダを初期化するため、接続のたびにbegin()の後で設定する
    webSocket.setAuthorization(authHeader.c_str());
    // キーの中身は出さず、設定されているかだけを出す
    Serial.printf("[Live] authorization header set (key length: %d)\n", authHeader.length() - 7);
}

void GptLive::closeSession()
{
    if(playing){
        endPlayback(false);
    }
    RealtimeLLMBase::stopRealtimeRecord();

    if(state == LIVE_ACTIVE){
        Serial.println("[Live] close session");
        sendControl("session.close");
        state = LIVE_CLOSING;
        stateStartMs = millis();
    }
    else if(state == LIVE_CONNECTING){
        releaseConnection();
    }
}

// 状態を未接続に戻す。以降webSocket.loop()を呼ばないので自動再接続もしない
void GptLive::releaseConnection()
{
    Serial.println("[Live] release connection");
    if(playing){
        endPlayback(false);
    }
    RealtimeLLMBase::stopRealtimeRecord();
    state = LIVE_IDLE;
    webSocket.disconnect();
    discardingBurst = false;
    lastBurstWasBackchannel = false;
    balloonUntilMs = millis();
    speaking = false;
}

void GptLive::sendSessionStart()
{
    SpiRamJsonDocument doc(1024*20);
    doc["type"] = "session.start";
    doc["event_id"] = nextEventId("start");
    JsonObject session = doc["session"].to<JsonObject>();
    session["model"] = liveModel;
    String delegationPolicy = live_delegation_instructions;
    if(enableMemory()){
        delegationPolicy += live_delegation_memory_instructions;
    }
    session["instructions"] = role + " " + delegationPolicy + " " + userInfo;
    JsonObject audio = session["audio"].to<JsonObject>();
    audio["format"]["type"] = "audio/pcm";
    audio["format"]["rate"] = GPT_LIVE_SAMPLE_RATE;
    audio["output"]["voice"] = liveVoice;

    // 関数の使い方の方針は、ツールを実行する委譲先モデルに渡す
    JsonObject responses = session["delegation"]["responses"].to<JsonObject>();
    session["delegation"]["type"] = "responses";
    responses["model"] = delegationModel;
    responses["instructions"] = systemRole;
    responses["tool_choice"] = "auto";
    JsonArray tools = responses["tools"].to<JsonArray>();

    // MCP tools listをfunctionとして挿入
    for(int s=0; s < param.llm_conf.nMcpServers; s++){
        if(true == param.llm_conf.mcpServer[s].disabled){
            continue;
        }
        if(!mcpClient[s]->isConnected()){
            continue;
        }
        for(int t=0; t < mcpClient[s]->nTools; t++){
            JsonObject tool = tools.add<JsonObject>();
            tool.set(mcpClient[s]->toolsListDoc["result"]["tools"][t].as<JsonObjectConst>());
            tool["type"] = "function";
        }
    }

    // FunctionCall.cppで定義したfunctionを挿入
    SpiRamJsonDocument functionsDoc(1024*10);
    DeserializationError error = deserializeJson(functionsDoc, json_Functions.c_str());
    if (error) {
        Serial.println("[Live] FunctionCall: JSON deserialization error");
    }
    for(JsonObject func : functionsDoc.as<JsonArray>()){
        JsonObject tool = tools.add<JsonObject>();
        tool.set(func);
        tool["type"] = "function";
    }

    String json;
    serializeJson(doc, json);
    Serial.printf("[Live] session.start: %s\n", json.c_str());
    webSocket.sendTXT(json);
}


void GptLive::beginPlayback()
{
    Serial.println("[Live] begin playback");
    RealtimeLLMBase::stopRealtimeRecord();
    sendControl("session.input_audio.mute");
    enterMutexAudio();
    M5.Mic.end();
    M5.Speaker.begin();
    speaking = true;
    playing = true;
}

void GptLive::endPlayback(bool resumeInput)
{
    Serial.println("[Live] end playback");
    while (M5.Speaker.isPlaying()) { vTaskDelay(1); }
    M5.Speaker.end();
    M5.Mic.begin();
    exitMutexAudio();
    clearAudioBuf();
    playing = false;
    speaking = false;
    lastActivityMs = millis();

    if(resumeInput){
        sendControl("session.input_audio.unmute");
        recordResumeMs = millis();
        RealtimeLLMBase::startRealtimeRecord();
    }
}

// GPT-Liveは話していない間も無音の出力音声を流し続けるため、振幅で声の有無を判定する。
// 声のまとまり（無音が途切れるまで）ごとに、相槌か応答かを決める。
// ユーザーの発話中に始まったまとまりは相槌とみなし、再生せず最後まで捨てる。
void GptLive::handleOutputAudio(String& delta)
{
    if(state != LIVE_ACTIVE || pcmScratch == NULL){
        return;
    }
    int base64Size = delta.length();
    if((base64Size / 4) * 3 + 1 > RT_AUDIO_BUF_SIZE){
        Serial.printf("[Live] audio delta too large, skipped: %d byte\n", base64Size);
        return;
    }
    int len = base64_decode(delta.c_str(), base64Size, (char*)pcmScratch);

    const int16_t* pcm = (const int16_t*)pcmScratch;
    int peak = 0;
    for(int i = 0; i < len / 2; i++){
        int v = abs(pcm[i]);
        if(v > peak) peak = v;
    }

    unsigned long now = millis();
    bool voiced = peak > GPT_LIVE_OUT_VOICE_LEVEL;
    bool newBurst = (now - lastVoicedOutMs) > GPT_LIVE_OUTPUT_END_MS;
    if(voiced){
        lastVoicedOutMs = now;
        lastActivityMs = now;   // 無音チャンクでは無操作タイマーを延長しない
    }

    if(!playing){
        if(!voiced){
            return;     // 待機中の無音は捨てる
        }
        if(newBurst || !discardingBurst){
            bool userSpeaking = isRealtimeRecording() && (now - lastVoiceMs) < GPT_LIVE_VAD_HOLD_MS;
            discardingBurst = userSpeaking;
            lastBurstWasBackchannel = userSpeaking;
            if(userSpeaking){
                Serial.printf("[Live] backchannel detected (out peak %d, mic peak %d %lums ago), skip playback\n",
                              peak, lastVoicePeak, now - lastVoiceMs);
                balloonBuf[0] = '\0';
                burstStartMs = now;
            }else{
                Serial.printf("[Live] voice detected (peak %d)\n", peak);
                beginPlayback();
            }
        }
        else if(discardingBurst && (now - burstStartMs) > GPT_LIVE_BACKCHANNEL_MAX_MS){
            // 相槌にしては長いので応答とみなし、ここから再生する（冒頭は欠ける）
            Serial.printf("[Live] backchannel too long (%lums), start playback\n", now - burstStartMs);
            discardingBurst = false;
            lastBurstWasBackchannel = false;
            beginPlayback();
        }
        if(!playing){
            return;
        }
    }

    // 応答の途中の間（無音）は再生に回して音をつなげる。無音が続いたら積むのをやめ、
    // 再生しきったところで onProcess() が録音に戻す
    if(voiced || !newBurst){
        queuePcm(pcmScratch, len);
    }
}

void GptLive::handleOutputTranscript(const char* delta)
{
    Serial.printf("[Live] output: %s\n", delta);
    lastActivityMs = millis();
    if(playing || !lastBurstWasBackchannel){
        return;
    }

    // 相槌は音声の代わりに吹き出しに表示する
    size_t len = strlen(balloonBuf);
    strlcpy(balloonBuf + len, delta, sizeof(balloonBuf) - len);
    balloonUntilMs = millis() + GPT_LIVE_BALLOON_HOLD_MS;
    avatar.setSpeechText(balloonBuf);
}

void GptLive::handleResponseEvent(JsonVariant event)
{
    const char* eventType = event["type"] | "";
    if(strcmp(eventType, "response.output_item.done") != 0){
        return;
    }
    JsonVariant item = event["item"];
    if(strcmp(item["type"] | "", "function_call") != 0){
        return;
    }

    const char* name = item["name"] | "";
    const char* args = item["arguments"] | "{}";
    const char* call_id = item["call_id"] | "";
    Serial.printf("[Live] function call name: %s, args: %s\n", name, args);
    lastActivityMs = millis();

    String result = fnCall->exec_calledFunc(name, args);

    JsonDocument outputDoc;
    outputDoc["result"] = result;
    String output;
    serializeJson(outputDoc, output);

    JsonDocument doc;
    doc["type"] = "response.item.create";
    doc["event_id"] = nextEventId("fn");
    doc["item"]["type"] = "function_call_output";
    doc["item"]["call_id"] = call_id;
    doc["item"]["output"] = output;
    String json;
    serializeJson(doc, json);
    Serial.printf("[Live] function output: %s\n", json.c_str());
    webSocket.sendTXT(json);
    sendControl("response.create");
}


// 相槌判定用の簡易VAD。雑音でも反応するため、無操作タイマーの延長には使わない
// （延長はサーバー側の session.input_transcript.delta で行う）。
void GptLive::onRecordChunk(const int16_t* buf, int len)
{
    int peak = 0;
    for(int i = 0; i < len; i++){
        int v = abs(buf[i]);
        if(v > peak) peak = v;
    }
    unsigned long now = millis();
    if(now - recordResumeMs < GPT_LIVE_VAD_GUARD_MS){
        return;
    }
    if(peak > GPT_LIVE_VAD_LEVEL){
        lastVoiceMs = now;
        lastVoicePeak = peak;
    }
}

void GptLive::onProcess()
{
    if(closeRequested){
        closeRequested = false;
        openRequested = false;
        closeSession();
    }
    else if(openRequested){
        openRequested = false;
        if(state == LIVE_IDLE){
            openSession();
        }
    }

    // webSocket.loop()は1回で受信フレームを1つしか処理しない。GPT-Liveは無音でも100msごとに
    // 音声を送ってくるため、録音中（1周約125ms）に1つずつでは追いつかず、受信が溜まって
    // 文字起こしや応答開始がどんどん遅れる。1周の中で時間の許す限り追加で処理する。
    unsigned long drainStart = millis();
    for(int i = 0; i < GPT_LIVE_WS_DRAIN_MAX && state != LIVE_IDLE; i++){
        if(millis() - drainStart > GPT_LIVE_WS_DRAIN_BUDGET_MS){
            break;
        }
        webSocket.loop();
    }

    unsigned long now = millis();

    switch(state){
    case LIVE_CONNECTING:
        if(now - stateStartMs > GPT_LIVE_CONNECT_TIMEOUT_MS){
            Serial.println("[Live] connect timeout");
            releaseConnection();
        }
        break;
    case LIVE_CLOSING:
        if(now - stateStartMs > GPT_LIVE_CLOSE_TIMEOUT_MS){
            Serial.println("[Live] session.closed timeout");
            releaseConnection();
        }
        break;
    case LIVE_ACTIVE:
        if(playing){
            lastActivityMs = now;
            if((now - lastVoicedOutMs) > GPT_LIVE_OUTPUT_END_MS && !M5.Speaker.isPlaying()){
                endPlayback(true);
            }
        }
        else if(now - lastActivityMs > GPT_LIVE_IDLE_TIMEOUT_MS){
            Serial.println("[Live] idle timeout");
            closeSession();
        }
        break;
    default:
        break;
    }
}

// Mod切替（RealtimeAiMod::pause()）でwebSocketLoopTaskが止まると、無操作タイマーもsession.closeも
// 動かず、接続時間で課金されるセッションが開いたままになる。止める前に閉じ終わるまで待つ。
// メインループのタスクから呼ばれるため、閉じる処理自体はwebSocketLoopTaskに要求して任せる。
void GptLive::beforeSuspend()
{
    openRequested = false;
    if(state == LIVE_IDLE){
        return;
    }
    Serial.println("[Live] close session before suspend");
    closeRequested = true;
    unsigned long start = millis();
    while(state != LIVE_IDLE && millis() - start < GPT_LIVE_SUSPEND_WAIT_MS){
        vTaskDelay(10);
    }
    if(state != LIVE_IDLE){
        Serial.println("[Live] close before suspend timed out");
    }
}

const char* GptLive::idleStatusText()
{
    if(state == LIVE_CONNECTING){
        return "Connecting...";
    }
    return "Please touch";
}


#endif  //REALTIME_API && !REALTIME_API_WITH_TTS
