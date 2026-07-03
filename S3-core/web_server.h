/**
 * web_server.h — WiFi 更新模式、HTTP 处理
 */
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "config.h"
#include "sd_manager.h"
#include "model_runner.h"

// ==================== Web 全局 ====================
WebServer server(WEB_SERVER_PORT);
bool      wifi_update_mode = false;
bool      model_updated    = false;

// ==================== HTML 页面 (PROGMEM) ====================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><title>ESP32 Model Manager</title>
<meta charset="utf-8"></head>
<body style="font-family:Arial;text-align:center;padding:20px">
<h2>ESP32 Model Manager</h2>
<p>Status: <span id="s">Loading...</span></p>
<p>Current: <span id="cm">-</span></p>
<p>Heap: <span id="h">-</span></p>
<h3>Upload Model</h3>
<p>Filename: <input id="fn" value="model" size="15">.tflite</p>
<input type="file" id="f" accept=".tflite"><br><br>
<button onclick="up()" style="padding:10px 20px;font-size:16px">Upload</button>
<h3>Models on SD</h3>
<div id="ml">Loading...</div>
<p id="m"></p>
<script>
function up(){var f=document.getElementById('f').files[0];if(!f)return;
var fn=document.getElementById('fn').value||'model';
var r=new FileReader();r.onload=function(e){
var h='';var v=new Uint8Array(e.target.result);
for(var i=0;i<v.length;i++)h+=('0'+v[i].toString(16)).slice(-2);
var x=new XMLHttpRequest();
x.open('POST','/upload?name='+encodeURIComponent(fn));
x.onload=function(){
document.getElementById('m').textContent=x.status===200?'OK: '+x.responseText:'Error:'+x.responseText;
loadModels();
};
x.send('model='+encodeURIComponent(h));
};r.readAsArrayBuffer(f);}
function loadModels(){fetch('/models').then(r=>r.json()).then(d=>{
var html='';for(var i=0;i<d.models.length;i++){
var m=d.models[i];html+='<p>['+i+'] '+m.name+' ('+m.size+'b)';
if(m.active)html+=' <b>ACTIVE</b>';
else html+=' <button onclick="sw('+i+')">Load</button>';
html+='</p>';}document.getElementById('ml').innerHTML=html||'(none)';
}).catch(()=>document.getElementById('ml').innerHTML='Error');}
function sw(i){fetch('/model/load?index='+i).then(r=>r.text()).then(t=>{
document.getElementById('m').textContent=t;loadModels();});}
setInterval(function(){fetch('/status').then(r=>r.json()).then(d=>
{document.getElementById('s').textContent=d.model_loaded?'Loaded':'No';
document.getElementById('cm').textContent=d.current_model||'-';
document.getElementById('h').textContent=d.free_heap+'b'})},3000);
loadModels();
</script></body></html>
)rawliteral";

// 前向声明
void handleRoot();
void handleStatus();
void handleModelsList();
void handleModelLoad();
void handleUpload();
void handleRestart();
void handleNotFound();

// ==================== WiFi 更新模式 ====================
void startWiFiUpdateMode() {
  if (wifi_update_mode) {
    Serial.println("WiFi update mode already running");
    Serial.printf("IP: http://%s\n", WiFi.localIP().toString().c_str());
    return;
  }

  if (strlen(wifi_ssid) == 0) {
    Serial.println("[WiFi] No WiFi config! Create /wifi.conf on SD (line1=SSID, line2=PASSWORD)");
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Already connected, starting Web server...");
  } else {
    Serial.printf("Connecting to WiFi: %s\n", wifi_ssid);
    WiFi.begin(wifi_ssid, wifi_password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi connection failed!");
      return;
    }
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/models", HTTP_GET, handleModelsList);
  server.on("/model/load", HTTP_GET, handleModelLoad);
  server.on("/upload", HTTP_POST, handleUpload);
  server.on("/upload", HTTP_GET, handleRoot);
  server.on("/restart", HTTP_GET, handleRestart);
  server.onNotFound(handleNotFound);

  server.begin();

  wifi_update_mode = true;
  Serial.println("\n===== WiFi Update Mode =====");
  Serial.printf("IP Address: http://%s\n", WiFi.localIP().toString().c_str());
  Serial.println("Open browser to upload model");
  Serial.println("============================");
}

void stopWiFiUpdateMode() {
  if (!wifi_update_mode) {
    Serial.println("WiFi update mode not running");
    return;
  }

  server.stop();
  wifi_update_mode = false;
  Serial.println("WiFi update mode stopped");
}

// ==================== HTTP 处理函数 ====================
void handleRoot() {
  char buf[1024];
  strcpy_P(buf, INDEX_HTML);
  server.send(200, "text/html", String(buf));
}

void handleStatus() {
  String json = "{";
  json += "\"model_loaded\":" + String(model_loaded ? "true" : "false") + ",";
  json += "\"current_model\":\"" + String(currentModelFile) + "\",";
  json += "\"current_model_index\":" + String(currentModelIndex) + ",";
  json += "\"model_count\":" + String(modelCount) + ",";
  json += "\"model_updated\":" + String(model_updated ? "true" : "false") + ",";
  json += "\"sd_ready\":" + String(sd_ready ? "true" : "false") + ",";
  json += "\"wifi_mode\":" + String(wifi_update_mode ? "true" : "false") + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"ip_address\":\"" + WiFi.localIP().toString() + "\",";

  if (sd_ready && SD.exists(MODEL_FILE)) {
    File f = SD.open(MODEL_FILE);
    if (f) {
      json += "\"model_size\":\"" + String(f.size()) + " bytes\"";
      f.close();
    } else {
      json += "\"model_size\":\"unknown\"";
    }
  } else {
    json += "\"model_size\":\"not found\"";
  }

  json += "}";
  server.send(200, "application/json", json);
}

void handleUpload() {
  Serial.println("[WiFi] Upload request received");

  String targetFile = "/model.tflite";
  if (server.hasArg("name")) {
    String name = server.arg("name");
    if (name.length() > 0) {
      if (!name.startsWith("/")) name = "/" + name;
      if (!name.endsWith(".tflite")) name += ".tflite";
      targetFile = name;
    }
  }

  if (server.hasArg("model")) {
    String modelData = server.arg("model");
    Serial.printf("[WiFi] Model data: %d chars -> %s\n", modelData.length(), targetFile.c_str());

    if (modelData.length() < 10) {
      Serial.println("[WiFi] Data too small");
      server.send(400, "text/plain", "Data too small");
      return;
    }

    uint8_t* buffer = (uint8_t*)malloc(modelData.length() / 2);
    if (!buffer) {
      server.send(500, "text/plain", "Memory error");
      return;
    }

    size_t dataSize = 0;
    size_t maxModelSize = 500000;
    for (size_t i = 0; i + 1 < modelData.length() && dataSize < maxModelSize; i += 2) {
      char hex[3] = { modelData.charAt(i), modelData.charAt(i + 1), 0 };
      buffer[dataSize++] = strtol(hex, NULL, 16);
    }

    Serial.printf("[WiFi] Decoded: %d bytes\n", dataSize);

    if (SD.exists(targetFile.c_str())) {
      SD.remove(targetFile.c_str());
    }
    File file = SD.open(targetFile.c_str(), FILE_WRITE);
    if (file) {
      size_t written = file.write(buffer, dataSize);
      file.close();
      if (written == dataSize) {
        Serial.printf("[WiFi] Model saved: %s (%zu bytes)\n", targetFile.c_str(), dataSize);
        scanModels();
        model_updated = true;
        server.send(200, "text/plain", ("Saved: " + targetFile).c_str());
      } else {
        server.send(500, "text/plain", "Write incomplete");
      }
    } else {
      server.send(500, "text/plain", "Cannot create file");
    }

    free(buffer);
  } else {
    server.send(400, "text/plain", "No model data");
  }
}

void handleRestart() {
  server.send(200, "text/plain", "Restarting...");
  delay(500);
  ESP.restart();
}

void handleModelsList() {
  String json = "{\"models\":[";
  for (int i = 0; i < modelCount; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"name\":\"" + String(modelList[i].filename) + "\",";
    json += "\"label\":\"" + String(modelList[i].label) + "\",";
    json += "\"size\":" + String(modelList[i].filesize) + ",";
    json += "\"active\":" + String(i == currentModelIndex ? "true" : "false");
    json += "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleModelLoad() {
  if (!server.hasArg("index")) {
    server.send(400, "text/plain", "Missing index parameter");
    return;
  }
  int idx = server.arg("index").toInt();
  Serial.printf("[WiFi] Load model #%d request\n", idx);

  model_loaded = false;
  if (loadModelByIndex(idx)) {
    model_loaded = true;
    server.send(200, "text/plain", ("Loaded: " + String(modelList[idx].filename)).c_str());
  } else {
    server.send(500, "text/plain", "Failed to load model");
  }
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

#endif
