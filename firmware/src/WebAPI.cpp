#include <ESP32WebServer.h>
#include <nvs.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPIFFS.h>
#include "WebAPI.h"
#include "Avatar.h"
#include "StackchanExConfig.h"
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
extern StackchanExConfig system_config;

ESP32WebServer server(80);

// C++11 multiline string constants are neato...
static const char HEAD[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html lang="ja">
<head>
  <meta charset="UTF-8">
  <title>AIｽﾀｯｸﾁｬﾝ</title>
</head>)KEWL";


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

IMPORT_FILE(.rodata, "home.html", home_html);
IMPORT_FILE(.rodata, "config.html", config_html);
IMPORT_FILE(.rodata, "config.js", config_js);
IMPORT_FILE(.rodata, "personalize.html", personalize_html);
IMPORT_FILE(.rodata, "personalize.js", personalize_js);
#if !defined(ARDUINO_M5STACK_ATOMS3R)
IMPORT_FILE(.rodata, "sdmanager.html", sdmanager_html);
IMPORT_FILE(.rodata, "sdmanager.js", sdmanager_js);
#endif

static const char* SPIFFS_SEC_CONFIG_PATH = "/SC_SecConfig.yaml";
static const char* SPIFFS_BASIC_CONFIG_PATH = "/SC_BasicConfig.yaml";
static const char* SPIFFS_EX_CONFIG_PATH = "/SC_ExConfig.yaml";

String read_text_file(fs::FS& fs, const char* path)
{
  File file = fs.open(path, FILE_READ);
  if(!file){
    return "";
  }
  String data = file.readString();
  file.close();
  return data;
}

bool write_text_file_atomic(fs::FS& fs, const char* path, const String& data, String* error)
{
  String temp_path = String(path) + ".tmp";
  File temp = fs.open(temp_path.c_str(), FILE_WRITE);
  if(!temp){
    if(error != nullptr){
      *error = String("Cannot open temp file: ") + temp_path;
    }
    return false;
  }
  size_t written = temp.print(data);
  temp.flush();
  temp.close();
  if(written != data.length()){
    fs.remove(temp_path.c_str());
    if(error != nullptr){
      *error = String("Failed to write full data: ") + path;
    }
    return false;
  }
  if(fs.exists(path)){
    fs.remove(path);
  }
  if(!fs.rename(temp_path.c_str(), path)){
    fs.remove(temp_path.c_str());
    if(error != nullptr){
      *error = String("Failed to rename temp file: ") + path;
    }
    return false;
  }
  return true;
}

bool parse_yaml_file(fs::FS& fs, const char* path, JsonDocument& doc)
{
  String yaml = read_text_file(fs, path);
  if(yaml.length() == 0){
    return false;
  }
  return !deserializeYml(doc, yaml.c_str());
}

// YAMLDuino の serializeYml() は文字列をクォートせずに出力し、"0123" や "true" のような
// 文字列が再読込時に数値/真偽値へ化けるため、スカラーは JSON 表記（YAML の上位互換）で出力する。
void append_yaml_indent(String& out, int indent)
{
  for(int i = 0; i < indent; i++){
    out += ' ';
  }
}

void append_yaml_scalar(String& out, JsonVariantConst value)
{
  if(value.isNull()){
    out += "\"\"";
    return;
  }
  String scalar;
  serializeJson(value, scalar);
  out += scalar;
}

void append_yaml_object(String& out, JsonObjectConst object, int indent);
void append_yaml_array(String& out, JsonArrayConst array, int indent);

// 値を "key: " や "- " の直後に続けて出力する。入れ子は改行して indent 位置から出力する。
void append_yaml_value(String& out, JsonVariantConst value, int indent)
{
  if(value.is<JsonObjectConst>() && value.as<JsonObjectConst>().size() > 0){
    out += '\n';
    append_yaml_object(out, value.as<JsonObjectConst>(), indent);
  }else if(value.is<JsonArrayConst>() && value.as<JsonArrayConst>().size() > 0){
    out += '\n';
    append_yaml_array(out, value.as<JsonArrayConst>(), indent);
  }else if(value.is<JsonObjectConst>()){
    out += "{}\n";
  }else if(value.is<JsonArrayConst>()){
    out += "[]\n";
  }else{
    append_yaml_scalar(out, value);
    out += '\n';
  }
}

void append_yaml_object(String& out, JsonObjectConst object, int indent)
{
  for(JsonPairConst pair : object){
    append_yaml_indent(out, indent);
    out += pair.key().c_str();
    out += ": ";
    append_yaml_value(out, pair.value(), indent + 2);
  }
}

void append_yaml_array(String& out, JsonArrayConst array, int indent)
{
  for(JsonVariantConst item : array){
    if(item.is<JsonObjectConst>() && item.as<JsonObjectConst>().size() > 0){
      // 1つ目のキーを "- " と同じ行に置くため、indent+2 で出力した先頭の空白を "- " に置き換える。
      String child;
      append_yaml_object(child, item.as<JsonObjectConst>(), indent + 2);
      append_yaml_indent(out, indent);
      out += "- ";
      out += child.substring(indent + 2);
    }else{
      append_yaml_indent(out, indent);
      out += "- ";
      append_yaml_value(out, item, indent + 2);
    }
  }
}

String json_to_yaml(JsonObjectConst root)
{
  String yaml;
  append_yaml_object(yaml, root, 0);
  return yaml;
}

String json_string_or_empty(JsonVariantConst value)
{
  if(value.is<const char*>()){
    return value.as<String>();
  }
  return "";
}

int json_int_or(JsonVariantConst value, int default_value)
{
  if(value.is<int>()){
    return value.as<int>();
  }
  return default_value;
}

bool json_bool_or(JsonVariantConst value, bool default_value)
{
  if(value.is<bool>()){
    return value.as<bool>();
  }
  return default_value;
}

bool validate_mcp_servers(JsonObjectConst llm, String* error)
{
  JsonVariantConst value = llm["mcpServers"];
  if(value.isNull()){
    return true;
  }
  if(!value.is<JsonArrayConst>()){
    *error = "llm.mcpServers must be an array";
    return false;
  }
  JsonArrayConst servers = value.as<JsonArrayConst>();
  if(servers.size() > LLM_N_MCP_SERVERS_MAX){
    *error = "llm.mcpServers must contain at most " + String(LLM_N_MCP_SERVERS_MAX) + " servers";
    return false;
  }
  int index = 0;
  for(JsonVariantConst item : servers){
    if(!item.is<JsonObjectConst>()){
      *error = "llm.mcpServers[" + String(index) + "] must be an object";
      return false;
    }
    JsonObjectConst server = item.as<JsonObjectConst>();
    if(!server["name"].is<const char*>() || server["name"].as<String>().length() == 0){
      *error = "llm.mcpServers[" + String(index) + "].name is required";
      return false;
    }
    if(!server["disabled"].is<bool>()){
      *error = "llm.mcpServers[" + String(index) + "].disabled must be true or false";
      return false;
    }
    if(!server["url"].is<const char*>() || server["url"].as<String>().length() == 0){
      *error = "llm.mcpServers[" + String(index) + "].url is required";
      return false;
    }
    if(!server["port"].is<int>()){
      *error = "llm.mcpServers[" + String(index) + "].port must be an integer";
      return false;
    }
    int port = server["port"].as<int>();
    if(port < 1 || port > 65535){
      *error = "llm.mcpServers[" + String(index) + "].port must be from 1 to 65535";
      return false;
    }
    index++;
  }
  return true;
}

JsonObject get_or_create_object(JsonObject parent, const char* key)
{
  if(parent[key].is<JsonObject>()){
    return parent[key].as<JsonObject>();
  }
  return parent[key].to<JsonObject>();
}

// SPIFFS 上の既存 YAML を読み込む。無い、または解析できない場合は空のオブジェクトから始める。
JsonObject load_yaml_for_merge(const char* path, JsonDocument& doc)
{
  if(!parse_yaml_file(SPIFFS, path, doc) || !doc.is<JsonObject>()){
    doc.clear();
    return doc.to<JsonObject>();
  }
  return doc.as<JsonObject>();
}

// Web UI が扱うキーだけを上書きし、それ以外のキーは既存の値を保持する。
void merge_secret_settings(JsonObject root, JsonObjectConst sec)
{
  JsonObjectConst wifi = sec["wifi"];
  JsonObjectConst apikey = sec["apikey"];
  JsonObject dst_wifi = get_or_create_object(root, "wifi");
  dst_wifi["ssid"] = json_string_or_empty(wifi["ssid"]);
  dst_wifi["password"] = json_string_or_empty(wifi["password"]);
  JsonObject dst_apikey = get_or_create_object(root, "apikey");
  dst_apikey["aiservice"] = json_string_or_empty(apikey["aiservice"]);
  dst_apikey["tts"] = json_string_or_empty(apikey["tts"]);
  dst_apikey["stt"] = json_string_or_empty(apikey["stt"]);
}

void merge_servo_axis(JsonObject servo, JsonObjectConst src, const char* key, int default_x, int default_y)
{
  JsonObject axis = get_or_create_object(servo, key);
  axis["x"] = json_int_or(src[key]["x"], default_x);
  axis["y"] = json_int_or(src[key]["y"], default_y);
}

void merge_basic_settings(JsonObject root, JsonObjectConst basic)
{
  JsonObjectConst servo = basic["servo"];
  JsonObject dst_servo = get_or_create_object(root, "servo");
  merge_servo_axis(dst_servo, servo, "pin", 33, 32);
  merge_servo_axis(dst_servo, servo, "offset", 0, 0);
  merge_servo_axis(dst_servo, servo, "center", 90, 90);
  merge_servo_axis(dst_servo, servo, "lower_limit", 0, 60);
  merge_servo_axis(dst_servo, servo, "upper_limit", 180, 90);
  root["takao_base"] = json_bool_or(basic["takao_base"], false);
  String servo_type = json_string_or_empty(basic["servo_type"]);
  root["servo_type"] = servo_type.length() > 0 ? servo_type : String("PWM");
}

void merge_extend_settings(JsonObject root, JsonObjectConst ex)
{
  JsonObjectConst llm = ex["llm"];
  JsonObject dst_llm = get_or_create_object(root, "llm");
  dst_llm["type"] = json_int_or(llm["type"], LLM_TYPE_CHATGPT);
  dst_llm["enableMemory"] = json_bool_or(llm["enableMemory"], false);
  // MCP サーバーは Web UI の入力が全件を表すため、配列ごと置き換える。
  JsonArray dst_servers = dst_llm["mcpServers"].to<JsonArray>();
  for(JsonObjectConst server : llm["mcpServers"].as<JsonArrayConst>()){
    JsonObject dst_server = dst_servers.add<JsonObject>();
    dst_server["name"] = server["name"].as<String>();
    dst_server["disabled"] = server["disabled"].as<bool>();
    dst_server["url"] = server["url"].as<String>();
    dst_server["port"] = server["port"].as<int>();
  }
}

bool build_merged_yaml(const char* path, JsonObjectConst input,
                       void (*merge)(JsonObject, JsonObjectConst), String* yaml, String* error)
{
  JsonDocument doc;
  JsonObject root = load_yaml_for_merge(path, doc);
  merge(root, input);
  if(doc.overflowed()){
    *error = String("Not enough memory to merge ") + path;
    return false;
  }
  *yaml = json_to_yaml(root);
  return true;
}

void handleRoot() {
  //Serial.println("handleRoot");
  //server.send(200, "text/plain", "hello from m5stack!");
  server.send_P(200, "text/html", (const char*)home_html, (size_t)sizeof_home_html);
}

void handle_config_html() {
  server.send_P(200, "text/html", (const char*)config_html, (size_t)sizeof_config_html);
}

void handle_config_js() {
  server.send_P(200, "application/javascript", (const char*)config_js, (size_t)sizeof_config_js);
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
  if(robot == nullptr){
    server.send(503, "text/plain", String("Robot is not ready"));
    return;
  }
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
  if(robot == nullptr){
    server.send(503, "text/plain", String("Robot is not ready"));
    return;
  }
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

void handle_role_set() {
  if(robot == nullptr || robot->llm == nullptr){
    server.send(503, "text/plain", String("LLM is not ready"));
    return;
  }
  String html = "";

  // POST以外は拒否
  if (server.method() != HTTP_POST) {
    return;
  }
  String role = server.arg("plain");

  // JSONデータをSPIFFSに保存
  if(robot->llm->save_userRole(role)){
    server.send(200, "text/plain", String("Role set successful"));
  }
  else{
    //html = "Failed to save role to SPIFFS.";
    server.send(500, "text/plain", String("Role set failed"));
  }
};

void handle_role_get() {
  if(robot == nullptr || robot->llm == nullptr){
    server.send(503, "text/plain", String("LLM is not ready"));
    return;
  }

  Serial.println("http request: handle_role_get");
  Serial.println(robot->llm->get_userRole());
  server.send(200, "text/plain", robot->llm->get_userRole());
};

void handle_memory_get() {
  if(robot == nullptr || robot->llm == nullptr){
    server.send(503, "text/plain", String("LLM is not ready"));
    return;
  }
  Serial.println("http request: handle_memory_get");
  Serial.println(robot->llm->get_userInfo());
  server.send(200, "text/plain", robot->llm->get_userInfo());
};

void handle_memory_clear() {
  if(robot == nullptr || robot->llm == nullptr){
    server.send(503, "text/plain", String("LLM is not ready"));
    return;
  }
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

void handle_config_get() {
  DynamicJsonDocument response(8192);
  bool has_sec = SPIFFS.exists(SPIFFS_SEC_CONFIG_PATH);
  bool has_basic = SPIFFS.exists(SPIFFS_BASIC_CONFIG_PATH);
  bool has_ex = SPIFFS.exists(SPIFFS_EX_CONFIG_PATH);
  response["source"] = (has_sec || has_basic || has_ex) ? "spiffs" : "none";
  response["complete"] = has_sec && has_basic && has_ex;

  JsonObject sec = response["sec"].to<JsonObject>();
  JsonObject wifi = sec["wifi"].to<JsonObject>();
  JsonObject apikey = sec["apikey"].to<JsonObject>();
  wifi["ssid"] = "";
  wifi["password"] = "";
  apikey["aiservice"] = "";
  apikey["tts"] = "";
  apikey["stt"] = "";
  DynamicJsonDocument sec_doc(2048);
  if(parse_yaml_file(SPIFFS, SPIFFS_SEC_CONFIG_PATH, sec_doc)){
    wifi["ssid"] = sec_doc["wifi"]["ssid"].as<String>();
    wifi["password"] = sec_doc["wifi"]["password"].as<String>();
    apikey["aiservice"] = sec_doc["apikey"]["aiservice"].as<String>();
    apikey["tts"] = sec_doc["apikey"]["tts"].as<String>();
    apikey["stt"] = sec_doc["apikey"]["stt"].as<String>();
  }else{
    secret_config_s* secret = system_config.getSecretSetting();
    if(secret != nullptr){
      wifi["ssid"] = secret->wifi_info.ssid;
      wifi["password"] = secret->wifi_info.password;
      apikey["aiservice"] = secret->api_key.ai_service;
      apikey["tts"] = secret->api_key.tts;
      apikey["stt"] = secret->api_key.stt;
    }
  }

  JsonObject basic = response["basic"].to<JsonObject>();
  JsonObject servo = basic["servo"].to<JsonObject>();
  servo["pin"]["x"] = 33;
  servo["pin"]["y"] = 32;
  servo["offset"]["x"] = 0;
  servo["offset"]["y"] = 0;
  servo["center"]["x"] = 90;
  servo["center"]["y"] = 90;
  servo["lower_limit"]["x"] = 0;
  servo["lower_limit"]["y"] = 60;
  servo["upper_limit"]["x"] = 180;
  servo["upper_limit"]["y"] = 90;
  basic["takao_base"] = false;
  basic["servo_type"] = "PWM";
  DynamicJsonDocument basic_doc(2048);
  if(parse_yaml_file(SPIFFS, SPIFFS_BASIC_CONFIG_PATH, basic_doc)){
    servo["pin"]["x"] = basic_doc["servo"]["pin"]["x"] | 33;
    servo["pin"]["y"] = basic_doc["servo"]["pin"]["y"] | 32;
    servo["offset"]["x"] = basic_doc["servo"]["offset"]["x"] | 0;
    servo["offset"]["y"] = basic_doc["servo"]["offset"]["y"] | 0;
    servo["center"]["x"] = basic_doc["servo"]["center"]["x"] | 90;
    servo["center"]["y"] = basic_doc["servo"]["center"]["y"] | 90;
    servo["lower_limit"]["x"] = basic_doc["servo"]["lower_limit"]["x"] | 0;
    servo["lower_limit"]["y"] = basic_doc["servo"]["lower_limit"]["y"] | 60;
    servo["upper_limit"]["x"] = basic_doc["servo"]["upper_limit"]["x"] | 180;
    servo["upper_limit"]["y"] = basic_doc["servo"]["upper_limit"]["y"] | 90;
    basic["takao_base"] = basic_doc["takao_base"] | false;
    basic["servo_type"] = basic_doc["servo_type"].as<String>().length() > 0
                         ? basic_doc["servo_type"].as<String>()
                         : String("PWM");
  }

  JsonObject ex = response["ex"].to<JsonObject>();
  JsonObject llm = ex["llm"].to<JsonObject>();
  llm["type"] = LLM_TYPE_CHATGPT;
  llm["enableMemory"] = false;
  JsonArray mcp_servers = llm["mcpServers"].to<JsonArray>();
  DynamicJsonDocument ex_doc(4096);
  if(parse_yaml_file(SPIFFS, SPIFFS_EX_CONFIG_PATH, ex_doc)){
    llm["type"] = ex_doc["llm"]["type"] | LLM_TYPE_CHATGPT;
    llm["enableMemory"] = ex_doc["llm"]["enableMemory"] | false;
    JsonArrayConst stored_servers = ex_doc["llm"]["mcpServers"].as<JsonArrayConst>();
    int count = 0;
    for(JsonObjectConst stored_server : stored_servers){
      if(count >= LLM_N_MCP_SERVERS_MAX){
        break;
      }
      JsonObject server = mcp_servers.createNestedObject();
      server["name"] = stored_server["name"].as<String>();
      server["disabled"] = stored_server["disabled"] | false;
      server["url"] = stored_server["url"].as<String>();
      server["port"] = stored_server["port"] | 0;
      count++;
    }
  }else{
    ex_config_s ex_config = system_config.getExConfig();
    if(ex_config.llm.type == LLM_TYPE_CHATGPT || ex_config.llm.type == LLM_TYPE_GEMINI){
      llm["type"] = ex_config.llm.type;
    }
    llm["enableMemory"] = ex_config.llm.enableMemory;
    for(int i = 0; i < ex_config.llm.nMcpServers && i < LLM_N_MCP_SERVERS_MAX; i++){
      JsonObject server = mcp_servers.createNestedObject();
      server["name"] = ex_config.llm.mcpServer[i].name;
      server["disabled"] = ex_config.llm.mcpServer[i].disabled;
      server["url"] = ex_config.llm.mcpServer[i].url;
      server["port"] = ex_config.llm.mcpServer[i].port;
    }
  }

  String json;
  serializeJson(response, json);
  server.send(200, "application/json", json);
}

void handle_config_post() {
  if(server.method() != HTTP_POST){
    server.send(405, "text/plain", String("Method Not Allowed"));
    return;
  }

  String body = server.arg("plain");
  String error = "";
  DynamicJsonDocument request(8192);
  DeserializationError parse_error = deserializeJson(request, body);
  if(parse_error){
    server.send(400, "text/plain", String("JSON parse error: ") + parse_error.c_str());
    return;
  }
  if(!validate_mcp_servers(request["ex"]["llm"], &error)){
    server.send(400, "text/plain", error);
    return;
  }

  // 3ファイルともマージ結果を作れた場合だけ書き込み、途中失敗で一部だけ更新されるのを避ける。
  String sec_yaml, basic_yaml, ex_yaml;
  if(!build_merged_yaml(SPIFFS_SEC_CONFIG_PATH, request["sec"], merge_secret_settings, &sec_yaml, &error)
     || !build_merged_yaml(SPIFFS_BASIC_CONFIG_PATH, request["basic"], merge_basic_settings, &basic_yaml, &error)
     || !build_merged_yaml(SPIFFS_EX_CONFIG_PATH, request["ex"], merge_extend_settings, &ex_yaml, &error)){
    server.send(500, "text/plain", error);
    return;
  }

  if(!system_config.saveSecretConfigYaml(SPIFFS, SPIFFS_SEC_CONFIG_PATH, sec_yaml, &error)){
    server.send(400, "text/plain", error.length() > 0 ? error : String("Invalid SC_SecConfig.yaml"));
    return;
  }
  if(!write_text_file_atomic(SPIFFS, SPIFFS_BASIC_CONFIG_PATH, basic_yaml, &error)){
    server.send(500, "text/plain", error.length() > 0 ? error : String("Failed to save SC_BasicConfig.yaml"));
    return;
  }
  if(!write_text_file_atomic(SPIFFS, SPIFFS_EX_CONFIG_PATH, ex_yaml, &error)){
    server.send(500, "text/plain", error.length() > 0 ? error : String("Failed to save SC_ExConfig.yaml"));
    return;
  }

  server.send(200, "application/json", String("{\"status\":\"ok\"}"));
}

void handle_config_restart() {
  server.send(200, "text/plain", String("Restarting"));
  delay(200);
  ESP.restart();
}


void init_web_server(bool enableSdManager)
{
  // Files
  //
  server.on("/", handleRoot);
  server.on("/config.html", handle_config_html);
  server.on("/config.js", handle_config_js);
  server.on("/personalize.html", handle_personalize_html);
  server.on("/personalize.js", handle_personalize_js);


  // APIs
  //
  server.on("/speech", handle_speech);
  server.on("/face", handle_face);
  server.on("/chat", handle_chat);
  server.on("/role_set", HTTP_POST, handle_role_set);
  server.on("/role_get", handle_role_get);
  server.on("/memory_get", handle_memory_get);
  server.on("/memory_clear", handle_memory_clear);
  server.on("/config", HTTP_GET, handle_config_get);
  server.on("/config", HTTP_POST, handle_config_post);
  server.on("/config/restart", HTTP_POST, handle_config_restart);

#if !defined(ARDUINO_M5STACK_ATOMS3R)
  // SD Card Manager
  // LAN内無認証でSDカード全体を読み書きできるため、SC_ExConfig.yamlのweb.sd_managerで
  // 明示的に有効化した時のみルートを登録する（無効時は未登録＝404）。
  if (enableSdManager) {
    server.on("/sdmanager.html", handle_sdmanager_html);
    server.on("/sdmanager.js", handle_sdmanager_js);
    server.on("/sd/list", handle_sd_list);
    server.on("/sd/download", handle_sd_download);
    server.on("/sd/upload", HTTP_POST, handle_sd_upload_done, handle_sd_upload);
    server.on("/sd/delete", HTTP_POST, handle_sd_delete);
  }
#endif

  // Other
  //
  server.onNotFound(handleNotFound);
  server.on("/inline", [](){
    server.send(200, "text/plain", "this works as well");
  });

  server.begin();
  Serial.println("HTTP server started");
  //M5.Lcd.println("HTTP server started");  
}

void web_server_handle_client(void)
{
  server.handleClient();
}
