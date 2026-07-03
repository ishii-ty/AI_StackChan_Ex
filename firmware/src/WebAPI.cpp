#include <ESP32WebServer.h>
#include <nvs.h>
#include <ArduinoJson.h>
#include "WebAPI.h"
#include "Avatar.h"
#include "llm/ChatGPT/ChatGPT.h"
#include "llm/ChatGPT/FunctionCall.h"
#include "Robot.h"
#if !defined(ARDUINO_M5STACK_ATOMS3R)
#include <SD.h>
#endif

using namespace m5avatar;
extern Avatar avatar;
extern uint8_t m5spk_virtual_channel;
extern String STT_API_KEY;

ESP32WebServer server(80);

// C++11 multiline string constants are neato...
static const char HEAD[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html lang="ja">
<head>
  <meta charset="UTF-8">
  <title>AIｽﾀｯｸﾁｬﾝ</title>
</head>)KEWL";

static const char APIKEY_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="UTF-8">
    <title>APIキー設定</title>
  </head>
  <body>
    <h1>APIキー設定</h1>
    <form>
      <label for="role1">OpenAI API Key</label>
      <input type="text" id="openai" name="openai" oninput="adjustSize(this)"><br>
      <label for="role2">VoiceVox API Key</label>
      <input type="text" id="voicevox" name="voicevox" oninput="adjustSize(this)"><br>
      <label for="role3">Speech to Text API Key</label>
      <input type="text" id="sttapikey" name="sttapikey" oninput="adjustSize(this)"><br>
      <button type="button" onclick="sendData()">送信する</button>
    </form>
    <script>
      function adjustSize(input) {
        input.style.width = ((input.value.length + 1) * 8) + 'px';
      }
      function sendData() {
        // FormDataオブジェクトを作成
        const formData = new FormData();

        // 各ロールの値をFormDataオブジェクトに追加
        const openaiValue = document.getElementById("openai").value;
        if (openaiValue !== "") formData.append("openai", openaiValue);

        const voicevoxValue = document.getElementById("voicevox").value;
        if (voicevoxValue !== "") formData.append("voicevox", voicevoxValue);

        const sttapikeyValue = document.getElementById("sttapikey").value;
        if (sttapikeyValue !== "") formData.append("sttapikey", sttapikeyValue);

	    // POSTリクエストを送信
	    const xhr = new XMLHttpRequest();
	    xhr.open("POST", "/apikey_set");
	    xhr.onload = function() {
	      if (xhr.status === 200) {
	        alert("データを送信しました！");
	      } else {
	        alert("送信に失敗しました。");
	      }
	    };
	    xhr.send(formData);
	  }
	</script>
  </body>
</html>)KEWL";

#if 0
static const char ROLE_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html>
<head>
	<title>ロール設定</title>
	<meta charset="UTF-8">
	<meta name="viewport" content="width=device-width, initial-scale=1.0">
	<style>
		textarea {
			width: 80%;
			height: 200px;
			resize: both;
		}
	</style>
</head>
<body>
	<h1>ロール設定</h1>
	<form onsubmit="postData(event)">
		<label for="textarea">ここにロールを記述してください。:</label><br>
		<textarea id="textarea" name="textarea"></textarea><br><br>
		<input type="submit" value="Submit">
	</form>
	<script>
		function postData(event) {
			event.preventDefault();
			const textAreaContent = document.getElementById("textarea").value.trim();
//			if (textAreaContent.length > 0) {
				const xhr = new XMLHttpRequest();
				xhr.open("POST", "/role_set", true);
				xhr.setRequestHeader("Content-Type", "text/plain;charset=UTF-8");
			// xhr.onload = () => {
			// 	location.reload(); // 送信後にページをリロード
			// };
			xhr.onload = () => {
				document.open();
				document.write(xhr.responseText);
				document.close();
			};
				xhr.send(textAreaContent);
//        document.getElementById("textarea").value = "";
				alert("Data sent successfully!");
//			} else {
//				alert("Please enter some text before submitting.");
//			}
		}
	</script>
</body>
</html>)KEWL";
#endif

#define IMPORT_FILE(section, filename, symbol) \
static constexpr const char* filename_##symbol = filename; \
extern const uint8_t symbol[], sizeof_##symbol[]; \
asm(\
  ".section " #section "\n"\
  ".balign 4\n"\
  ".global " #symbol "\n"\
  #symbol ":\n"\
  ".incbin \"incbin/" filename "\"\n"\
  ".global sizeof_" #symbol "\n"\
  ".set sizeof_" #symbol ", . - " #symbol "\n"\
  ".balign 4\n"\
  ".section \".text\"\n")

//IMPORT_FILE(.rodata, "index.html", index_html);
IMPORT_FILE(.rodata, "personalize.html", personalize_html);
IMPORT_FILE(.rodata, "personalize.js", personalize_js);
#if !defined(ARDUINO_M5STACK_ATOMS3R)
IMPORT_FILE(.rodata, "sdmanager.html", sdmanager_html);
IMPORT_FILE(.rodata, "sdmanager.js", sdmanager_js);
#endif


void handleRoot() {
  //Serial.println("handleRoot");
  //server.send(200, "text/plain", "hello from m5stack!");
  server.send_P(200, "text/html", (const char*)personalize_html, (size_t)sizeof_personalize_html);
}

void handle_personalize_html() {
  server.send_P(200, "text/html", (const char*)personalize_html, (size_t)sizeof_personalize_html);
}

void handle_personalize_js() {
  server.send_P(200, "application/javascript", (const char*)personalize_js, (size_t)sizeof_personalize_js);
}

void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
//  server.send(404, "text/plain", message);
  server.send(404, "text/html", String(HEAD) + String("<body>") + message + String("</body>"));
}

void handle_speech() {
  String message = server.arg("say");
  String speaker = server.arg("voice");
  //if(speaker != "") {
  //  TTS_PARMS = TTS_SPEAKER + speaker;
  //}
  Serial.println(message);
  ////////////////////////////////////////
  // 音声の発声
  ////////////////////////////////////////
  //avatar.setExpression(Expression::Happy);
  robot->speech(message);
  server.send(200, "text/plain", String("OK"));
}

void handle_chat() {
  static String response = "";
  // tts_parms_no = 1;
  String text = server.arg("text");
  String speaker = server.arg("voice");
  //if(speaker != "") {
  //  TTS_PARMS = TTS_SPEAKER + speaker;
  //}

  robot->chat(text);

  server.send(200, "text/html", String(HEAD)+String("<body>")+response+String("</body>"));
}

void handle_apikey() {
  // ファイルを読み込み、クライアントに送信する
  server.send(200, "text/html", APIKEY_HTML);
}

#if 0
void handle_apikey_set() {
  // POST以外は拒否
  if (server.method() != HTTP_POST) {
    return;
  }
  // openai
  String openai = server.arg("openai");
  // voicetxt
  String voicevox = server.arg("voicevox");
  // voicetxt
  String sttapikey = server.arg("sttapikey");
 
  OPENAI_API_KEY = openai;
  VOICEVOX_API_KEY = voicevox;
  STT_API_KEY = sttapikey;
  Serial.println(openai);
  Serial.println(voicevox);
  Serial.println(sttapikey);

  uint32_t nvs_handle;
  if (ESP_OK == nvs_open("apikey", NVS_READWRITE, &nvs_handle)) {
    nvs_set_str(nvs_handle, "openai", openai.c_str());
    nvs_set_str(nvs_handle, "voicevox", voicevox.c_str());
    nvs_set_str(nvs_handle, "sttapikey", sttapikey.c_str());
    nvs_close(nvs_handle);
  }
  server.send(200, "text/plain", String("OK"));
}
#endif

void handle_role_set() {
  String html = "";

  // POST以外は拒否
  if (server.method() != HTTP_POST) {
    return;
  }
  String role = server.arg("plain");

  // JSONデータをSPIFFSに保存
  if(robot->llm->save_userRole(role)){
#if 0
    // 整形したJSONデータを出力するHTMLデータを作成する
    serializeJsonPretty(robot->llm->get_chat_doc(), html);
    html = "<html><body><pre>" + html + "</pre></body></html>";
    //Serial.println(html);
#endif
    server.send(200, "text/plain", String("Role set successful"));
  }
  else{
    //html = "Failed to save role to SPIFFS.";
    server.send(500, "text/plain", String("Role set failed"));
  }

  // HTMLデータをシリアルに出力する
  //server.send(200, "text/html", html);
};

void handle_role_get() {
#if 0
  String html = "";
  serializeJsonPretty(robot->llm->get_chat_doc(), html);
  html = "<html><body><pre>" + html + "</pre></body></html>";

  // HTMLデータをシリアルに出力する
  //Serial.println(html);
  server.send(200, "text/html", String(HEAD) + html);
#endif
  Serial.println("http request: handle_role_get");
  Serial.println(robot->llm->get_userRole());
  server.send(200, "text/plain", robot->llm->get_userRole());
};

void handle_memory_get() {
  Serial.println("http request: handle_memory_get");
  Serial.println(robot->llm->get_userInfo());
  server.send(200, "text/plain", robot->llm->get_userInfo());
};

void handle_memory_clear() {
  Serial.println("http request: handle_memory_clear");
  bool result = robot->llm->clear_userInfo();
  if(result){
    server.send(200, "text/plain", String("Memory clear successful"));
  }else{
    server.send(500, "text/plain", String("Memory clear failed"));
  }
};

void handle_face() {
  String expression = server.arg("expression");
  expression = expression + "\n";
  Serial.println(expression);
  switch (expression.toInt())
  {
    case 0: avatar.setExpression(Expression::Neutral); break;
    case 1: avatar.setExpression(Expression::Happy); break;
    case 2: avatar.setExpression(Expression::Sleepy); break;
    case 3: avatar.setExpression(Expression::Doubt); break;
    case 4: avatar.setExpression(Expression::Sad); break;
    case 5: avatar.setExpression(Expression::Angry); break;  
  } 
  server.send(200, "text/plain", String("OK"));
}

#if !defined(ARDUINO_M5STACK_ATOMS3R)
// SD Card Manager
//

void handle_sdmanager_html() {
  server.send_P(200, "text/html", (const char*)sdmanager_html, (size_t)sizeof_sdmanager_html);
}

void handle_sdmanager_js() {
  server.send_P(200, "application/javascript", (const char*)sdmanager_js, (size_t)sizeof_sdmanager_js);
}

// dir/path が "/" 始まりで ".." を含まないことを確認する（パストラバーサル対策）
bool isSafeSdPath(const String& path) {
  return path.length() > 0 && path.startsWith("/") && path.indexOf("..") < 0;
}

// SD.begin() を最大 retries 回リトライする。バス競合による一時的なマウント失敗を吸収する。
bool sdBeginRetry(int retries = 3) {
  for (int i = 0; i < retries; i++) {
    if (SD.begin(GPIO_NUM_4, SPI, 25000000)) {
      return true;
    }
    delay(20);
  }
  return false;
}

// len バイトすべてを書き込む。短い書き込みが起きたら残りをリトライする。
bool sdWriteAllRetry(File& f, const uint8_t* buf, size_t len) {
  size_t off = 0;
  int retries = 3;
  while (off < len && retries-- > 0) {
    size_t w = f.write(buf + off, len - off);
    off += w;
  }
  return off == len;
}

String sdContentType(const String& path) {
  if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".yaml") || path.endsWith(".yml") || path.endsWith(".txt")) return "text/plain";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".gif")) return "image/gif";
  if (path.endsWith(".mp3")) return "audio/mpeg";
  if (path.endsWith(".wav")) return "audio/wav";
  return "application/octet-stream";
}

void handle_sd_list() {
  String dir = server.arg("dir");
  if (dir.length() == 0) dir = "/";
  if (!isSafeSdPath(dir)) {
    server.send(400, "text/plain", "Invalid path");
    return;
  }
  if (!sdBeginRetry()) {
    server.send(500, "text/plain", "SD mount failed");
    return;
  }
  File root = SD.open(dir);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    server.send(404, "text/plain", "Directory not found");
    return;
  }

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    String name = entry.name();
    int slashIdx = name.lastIndexOf('/');
    if (slashIdx >= 0) name = name.substring(slashIdx + 1);
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = name;
    obj["isDir"] = entry.isDirectory();
    obj["size"] = entry.isDirectory() ? 0 : entry.size();
    entry.close();
  }
  root.close();

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handle_sd_download() {
  String path = server.arg("path");
  if (!isSafeSdPath(path)) {
    server.send(400, "text/plain", "Invalid path");
    return;
  }
  if (!sdBeginRetry()) {
    server.send(500, "text/plain", "SD mount failed");
    return;
  }
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(404, "text/plain", "File not found");
    return;
  }
  String filename = path;
  int slashIdx = filename.lastIndexOf('/');
  if (slashIdx >= 0) filename = filename.substring(slashIdx + 1);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server.streamFile(file, sdContentType(path));
  file.close();
}

static File sdUploadFile;
static bool sdUploadOk = false;
static bool sdUploadActive = false;      // START で正常にファイルを開けたか（END/ABORTED での SD 後始末の可否）
static String sdUploadTempPath;
static String sdUploadFinalPath;

// アップロードファイル名にパス区切りや ".." を含まないことを確認する（dir と結合してもディレクトリ外に書き込めないようにする）
bool isSafeSdFilename(const String& name) {
  return name.length() > 0 && name.indexOf('/') < 0 && name.indexOf('\\') < 0 && name.indexOf("..") < 0;
}

void handle_sd_upload_done() {
  if (sdUploadOk) {
    server.send(200, "text/plain", "OK");
  } else {
    server.send(500, "text/plain", "Upload failed");
  }
}

// 一時ファイルの内容を最終ファイルへコピーしてから一時ファイルを削除する。
// SD.remove() の直後に SD.rename() を呼ぶと稀にリネームが失敗するため、
// 常に成功する open/write の組み合わせだけで置き換える。
bool replaceSdFile(const String& tempPath, const String& finalPath) {
  File src = SD.open(tempPath, FILE_READ);
  if (!src) {
    return false;
  }
  if (SD.exists(finalPath)) {
    SD.remove(finalPath);
  }
  File dst = SD.open(finalPath, FILE_WRITE);
  if (!dst) {
    src.close();
    return false;
  }
  bool ok = true;
  uint8_t buf[512];
  int n;
  while ((n = src.read(buf, sizeof(buf))) > 0) {
    if (!sdWriteAllRetry(dst, buf, (size_t)n)) {
      ok = false;
      break;
    }
  }
  src.close();
  dst.close();
  SD.remove(tempPath);
  return ok;
}

// アップロードは START→WRITE...→END(または ABORTED) が 1 回の handleClient() 内で
// 同期的に呼ばれる。
void handle_sd_upload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    sdUploadOk = false;
    sdUploadActive = false;
    String dir = server.arg("dir");
    if (dir.length() == 0) dir = "/";
    if (!isSafeSdPath(dir) || !isSafeSdFilename(upload.filename)) {
      return;
    }
    if (!sdBeginRetry()) {
      return;
    }
    if (!dir.endsWith("/")) dir += "/";
    sdUploadFinalPath = dir + upload.filename;
    // 同名ファイルへ上書き中に失敗しても元ファイルが失われないよう、一時ファイルに書き込んでから完了時に差し替える
    sdUploadTempPath = sdUploadFinalPath + ".uploading";
    // 既存ファイルに直接 FILE_WRITE で上書きすると 0 バイトになることがあるため、事前に削除してから新規作成する
    if (SD.exists(sdUploadTempPath)) {
      SD.remove(sdUploadTempPath);
    }
    sdUploadFile = SD.open(sdUploadTempPath, FILE_WRITE);
    sdUploadOk = (bool)sdUploadFile;
    sdUploadActive = sdUploadOk;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (sdUploadActive && sdUploadFile) {
      if (!sdWriteAllRetry(sdUploadFile, upload.buf, upload.currentSize)) {
        sdUploadOk = false;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (sdUploadFile) {
      sdUploadFile.close();
    }
    if (sdUploadActive) {
      if (sdUploadOk) {
        sdUploadOk = replaceSdFile(sdUploadTempPath, sdUploadFinalPath);
      } else {
        SD.remove(sdUploadTempPath);
      }
    }
    sdUploadActive = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    sdUploadOk = false;
    if (sdUploadFile) {
      sdUploadFile.close();
    }
    if (sdUploadActive) {
      SD.remove(sdUploadTempPath);
    }
    sdUploadActive = false;
  }
}

void handle_sd_delete() {
  String path = server.arg("path");
  if (!isSafeSdPath(path)) {
    server.send(400, "text/plain", "Invalid path");
    return;
  }
  if (!sdBeginRetry()) {
    server.send(500, "text/plain", "SD mount failed");
    return;
  }
  File f = SD.open(path);
  bool isDir = f && f.isDirectory();
  if (f) f.close();
  bool ok = isDir ? SD.rmdir(path) : SD.remove(path);
  server.send(ok ? 200 : 500, "text/plain", ok ? "Deleted" : "Delete failed");
}
#endif  //ARDUINO_M5STACK_ATOMS3R

#if 0
void handle_setting() {
  String value = server.arg("volume");
  String led = server.arg("led");
  String speaker = server.arg("speaker");
//  volume = volume + "\n";
  Serial.println(speaker);
  Serial.println(value);
  size_t speaker_no;

  if(speaker != ""){
    speaker_no = speaker.toInt();
    if(speaker_no > 60) {
      speaker_no = 60;
    }
    TTS_SPEAKER_NO = String(speaker_no);
    TTS_PARMS = TTS_SPEAKER + TTS_SPEAKER_NO;
  }

  if(value == "") value = "180";
  size_t volume = value.toInt();
  uint8_t led_onoff = 0;
  uint32_t nvs_handle;
  if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
    if(volume > 255) volume = 255;
    nvs_set_u32(nvs_handle, "volume", volume);
    if(led != "") {
      if(led == "on") led_onoff = 1;
      else  led_onoff = 0;
      nvs_set_u8(nvs_handle, "led", led_onoff);
    }
    nvs_set_u8(nvs_handle, "speaker", speaker_no);

    nvs_close(nvs_handle);
  }
  M5.Speaker.setVolume(volume);
  M5.Speaker.setChannelVolume(m5spk_virtual_channel, volume);
  server.send(200, "text/plain", String("OK"));
}
#endif


void init_web_server(void)
{
  // Files
  //
  server.on("/", handleRoot);
  server.on("/personalize.html", handle_personalize_html);
  server.on("/personalize.js", handle_personalize_js);


  // APIs
  //
  server.on("/speech", handle_speech);
  server.on("/face", handle_face);
  server.on("/chat", handle_chat);
  server.on("/apikey", handle_apikey);
  //server.on("/setting", handle_setting);
  //server.on("/apikey_set", HTTP_POST, handle_apikey_set);
  server.on("/role_set", HTTP_POST, handle_role_set);
  server.on("/role_get", handle_role_get);
  server.on("/memory_get", handle_memory_get);
  server.on("/memory_clear", handle_memory_clear);

#if !defined(ARDUINO_M5STACK_ATOMS3R)
  // SD Card Manager
  //
  server.on("/sdmanager.html", handle_sdmanager_html);
  server.on("/sdmanager.js", handle_sdmanager_js);
  server.on("/sd/list", handle_sd_list);
  server.on("/sd/download", handle_sd_download);
  server.on("/sd/upload", HTTP_POST, handle_sd_upload_done, handle_sd_upload);
  server.on("/sd/delete", HTTP_POST, handle_sd_delete);
#endif

  // Other
  //
  server.onNotFound(handleNotFound);
  server.on("/inline", [](){
    server.send(200, "text/plain", "this works as well");
  });

  server.begin();
  Serial.println("HTTP server started");
  M5.Lcd.println("HTTP server started");  
}

void web_server_handle_client(void)
{
  server.handleClient();
}
