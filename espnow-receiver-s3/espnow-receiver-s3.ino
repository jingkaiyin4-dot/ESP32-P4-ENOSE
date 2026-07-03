/**
 * ESP-NOW 接收端 - 智能食材新鲜度监测系统
 * 使用 TensorFlowLite_ESP32@0.8.0 库
 * 支持WiFi模型更新功能
 */

#include <esp_now.h>
#include <WiFi.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <Arduino.h>
#include <WebServer.h>
#include <Update.h>

// TensorFlowLite_ESP32
#include <TensorFlowLite_ESP32.h>
#include <tensorflow/lite/experimental/micro/kernels/all_ops_resolver.h>
#include <tensorflow/lite/experimental/micro/micro_error_reporter.h>
#include <tensorflow/lite/experimental/micro/micro_interpreter.h>
#include <tensorflow/lite/schema/schema_generated.h>
#include <tensorflow/lite/version.h>

using namespace tflite;

// PSRAM 支持 - 外挂 OPI PSRAM 强制启用
#include <esp_heap_caps.h>
#include <esp_psram.h>

// 强制启用 PSRAM 宏（外挂 PSRAM 需要）
#ifndef BOARD_HAS_PSRAM
#define BOARD_HAS_PSRAM 1
#endif

// ==================== 数据结构 ====================
#define STATUS_ADS1115_OK 0x01
#define STATUS_MHZ19C_OK 0x02
#define STATUS_BME680_OK 0x04
#define PKT_TYPE_SENSOR 0x01
#define PKT_TYPE_WARMUP 0x02
#define PKT_TYPE_COMMAND 0x03

typedef struct __attribute__((packed)) {
  uint8_t dataType;
  float odor_ppm;
  float hcho_ppm;
  float co_ppm;
  float voc_ppm;
  uint16_t co2_ppm;
  int16_t co2_temp;
  float env_temp;
  float humidity;
  uint8_t sensor_status;
  uint32_t timestamp;
} SensorData;

typedef struct __attribute__((packed)) {
  uint8_t dataType;
  uint16_t remainingSec;
  uint32_t timestamp;
} WarmupStatus;

typedef struct __attribute__((packed)) {
  uint8_t dataType;
  char command[32];
  uint32_t timestamp;
} CommandPacket;

// ==================== 配置 ====================
uint8_t sensorMac[] = {0x90, 0xE5, 0xB1, 0xCC, 0x3C, 0x78};
const unsigned long CONNECTION_TIMEOUT = 45000;
const unsigned long STARTUP_GRACE = 120000;
const int OFFLINE_CONFIRM_COUNT = 2;

// WiFi更新配置（从SD卡 /wifi.conf 读取，格式: SSID\nPASSWORD）
#define WIFI_CONF_FILE "/wifi.conf"
#define WEB_SERVER_PORT 80
char wifi_ssid[64] = "406-嵌入式创新实验室";
char wifi_password[64] = "406qrscxsys";

// SD卡管脚
#define SD_SCK  12
#define SD_MISO 14
#define SD_MOSI 13
#define SD_CS   15
#define MODEL_FILE "/model.tflite"
#define MODEL_DIR  "/"
#define MAX_MODELS 8
#define TENSOR_ARENA_SIZE 100000

// 多模型支持
struct ModelInfo {
  char filename[32];
  char label[32];    // 模型标签（用户自定义名称）
  size_t filesize;
};
ModelInfo modelList[MAX_MODELS];
int modelCount = 0;
int currentModelIndex = 0;  // 当前加载的模型索引
char currentModelFile[32] = "/model.tflite";

// ==================== 全局变量 ====================
unsigned long startTime = 0;
SensorData latestData;
bool hasValidSensorData = false;

struct PeerMonitor {
  uint8_t mac[6];
  unsigned long lastSeen;
  bool wasConnected;
  char name[16];
};
PeerMonitor peers[4];
int peerCount = 0;

// 模型类别定义 - 动态从SD卡加载
#define MAX_CLASSES 20
const char* classNames[MAX_CLASSES];
int numClasses = 0;
char classNamesBuffer[MAX_CLASSES][32];  // 最多20类，每类最多31字符

// 默认类别（备用）
const char* defaultClassNames[] = {
  "air",
  "apple_fresh", "apple_rotten", "apple_stale",
  "banana_fresh", "banana_rotten", "banana_stale",
  "chocolate_fresh", "chocolate_rotten", "chocolate_stale",
  "general_fresh", "general_rotten", "general_stale"
};
const int defaultNumClasses = 13;

// TensorFlow Lite
namespace {
  MicroErrorReporter micro_error_reporter;
  ops::micro::AllOpsResolver resolver;
}
const Model* tflite_model = nullptr;
MicroInterpreter* interpreter = nullptr;
alignas(16) uint8_t tensor_arena[TENSOR_ARENA_SIZE];
uint8_t* model_buffer = nullptr;  // 持有模型数据，切换时释放旧的

bool model_loaded = false;
float last_confidence = 0.0f;
int last_predicted = 0;

SPIClass *sd_spi = nullptr;
bool sd_ready = false;
File data_file;

// 空气基准状态管理（用于双任务模型）
bool baselineValid = false;
float airBaseline[5] = {0, 0, 0, 0, 0};  // 空气时的5传感器读数
int baselineCount = 0;
const int BASELINE_SAMPLES = 20;  // 采集20个样本取平均

// 归一化参数结构体
struct NormParams {
  float min[10];
  float max[10];
  bool loaded;
};
extern NormParams normParams;

// WiFi更新相关
WebServer server(WEB_SERVER_PORT);
bool wifi_update_mode = false;
bool model_updated = false;

// ====== 紫外消杀与 OTA 全局定义 ======
#define RELAY_PIN 14
bool uvState = false;
bool uvAutoMode = false;
unsigned long uvOnDuration = 30000; // 30秒
unsigned long uvStartTime = 0;
int uvThreshold = 70; // 默认新鲜度低于 70 分自动开启消杀

// ====== 雾化加湿器与风扇定义 ======
#define FOG_PIN 4
#define FAN_PIN 6

bool fogState = false;
bool fogAutoMode = false;
unsigned long fogOnDuration = 60000; // 60秒
unsigned long fogStartTime = 0;

bool fanState = false;
bool fanAutoMode = false;
unsigned long fanOnDuration = 120000; // 120秒
unsigned long fanStartTime = 0;


struct ModelMeta {
  char name[64];
  int total_chunks;
  int total_size;
  char version[32];
};

ModelMeta otaMeta;
File otaWriteFile;
bool ota_in_progress = false;
int expected_chunk = 0;

// 将大写 16 进制字符转换为字节
static uint8_t hexToByte(char hi, char lo) {
  auto cvt = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
  };
  return (cvt(hi) << 4) | cvt(lo);
}

// 逆向解析 model_update (支持空格和下划线两种格式)
// P4 发送: "model update name chunks size version" (空格)
// 旧格式:   "model_update_name_chunks_size_version" (下划线)
bool parseModelUpdate(const char* input, ModelMeta* out) {
  int prefixLen = 0;
  if (strncmp(input, "model_update_", 13) == 0) {
    prefixLen = 13;
  } else if (strncmp(input, "model update ", 13) == 0) {
    prefixLen = 13;
  } else {
    return false;
  }
  char temp[256];
  strncpy(temp, input + prefixLen, sizeof(temp) - 1);
  temp[sizeof(temp) - 1] = '\0';
  
  // 统一将空格转为下划线，方便后续统一按 '_' 解析
  for (char *p = temp; *p; p++) {
    if (*p == ' ') *p = '_';
  }
  
  char* p_ver = strrchr(temp, '_');
  if (!p_ver) return false;
  *p_ver = '\0';
  strncpy(out->version, p_ver + 1, sizeof(out->version) - 1);
  
  char* p_size = strrchr(temp, '_');
  if (!p_size) return false;
  *p_size = '\0';
  out->total_size = atoi(p_size + 1);
  
  char* p_chunks = strrchr(temp, '_');
  if (!p_chunks) return false;
  *p_chunks = '\0';
  out->total_chunks = atoi(p_chunks + 1);
  
  strncpy(out->name, temp, sizeof(out->name) - 1);
  return true;
}

// 解析 model_chunk (支持空格和下划线两种格式)
// P4 发送: "model chunk <id> <hex>" (空格)
// 旧格式:   "model_chunk_<id>_<hex>" (下划线)
bool parseModelChunk(const char* input, int* chunk_id, char* hex_data, size_t hex_max_len) {
  int prefixLen = 0;
  if (strncmp(input, "model_chunk_", 12) == 0) {
    prefixLen = 12;
  } else if (strncmp(input, "model chunk ", 12) == 0) {
    prefixLen = 12;
  } else {
    return false;
  }
  char temp[512];
  strncpy(temp, input + prefixLen, sizeof(temp) - 1);
  temp[sizeof(temp) - 1] = '\0';
  
  // 统一将空格转为下划线，方便后续查找分隔符
  for (char *p = temp; *p; p++) {
    if (*p == ' ') *p = '_';
  }
  
  char* p_hex = strchr(temp, '_');
  if (!p_hex) return false;
  *p_hex = '\0';
  
  *chunk_id = atoi(temp);
  strncpy(hex_data, p_hex + 1, hex_max_len - 1);
  hex_data[hex_max_len - 1] = '\0';
  return true;
}

// 向 Bridge/P4 反馈 JSON ACK
void sendOtaAck(const uint8_t* mac, const char* mt, int mid) {
  char json[64];
  if (strcmp(mt, "model_done") == 0 || strcmp(mt, "model_fail") == 0) {
    snprintf(json, sizeof(json), "{\"mt\":\"%s\"}", mt);
  } else {
    snprintf(json, sizeof(json), "{\"mt\":\"%s\",\"mid\":%d}", mt, mid);
  }
  
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }
  
  esp_now_send(mac, (uint8_t*)json, strlen(json) + 1);
  Serial.printf("[OTA] Sent ACK to Bridge: %s\n", json);
}

// 向 Bridge/P4 反馈设备状态
void sendDeviceStatus(const uint8_t* mac, const char* device, bool state, bool auto_mode, unsigned long duration, unsigned long start_time) {
  char json[128];
  int remain = 0;
  if (state && start_time > 0) {
    long elapsed = (millis() - start_time);
    remain = (duration - elapsed) / 1000;
    if (remain < 0) remain = 0;
  }
  snprintf(json, sizeof(json), "{\"type\":\"%s_status\",\"state\":%d,\"auto\":%d,\"remain\":%d}", 
           device, state ? 1 : 0, auto_mode ? 1 : 0, remain);
  
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }
  esp_now_send(mac, (uint8_t*)json, strlen(json) + 1);
  Serial.printf("[%s] Sent status to Bridge: %s\n", device, json);
}


// 命令与分片接收处理总入口
void handleCommandPacket(const char* packet, const uint8_t* src_mac) {
  Serial.printf("[CMD] Received: %s\n", packet);

  // 1. 处理元数据握手
  ModelMeta meta;
  if (parseModelUpdate(packet, &meta)) {
    otaMeta = meta;
    ota_in_progress = true;
    expected_chunk = 0;
    if (otaWriteFile) otaWriteFile.close();
    
    String tmpPath = "/" + String(meta.name) + ".tflite.tmp";
    if (SD.exists(tmpPath.c_str())) SD.remove(tmpPath.c_str());
    
    otaWriteFile = SD.open(tmpPath.c_str(), FILE_WRITE);
    if (otaWriteFile) {
      Serial.printf("[OTA] Temp file created: %s. Expecting %d chunks.\n", tmpPath.c_str(), meta.total_chunks);
      sendOtaAck(src_mac, "chunk_ok", 0);
    } else {
      Serial.println("[OTA] Failed to open temp file for write");
      sendOtaAck(src_mac, "model_fail", 0);
      ota_in_progress = false;
    }
    return;
  }

  // 2. 处理分片数据
  int chunk_id = -1;
  char hex_data[512] = {0};
  if (parseModelChunk(packet, &chunk_id, hex_data, sizeof(hex_data))) {
    if (!ota_in_progress || !otaWriteFile) {
      sendOtaAck(src_mac, "model_fail", 0);
      return;
    }
    if (chunk_id != expected_chunk) {
      Serial.printf("[OTA] Sequence error. Expected %d, got %d\n", expected_chunk, chunk_id);
      sendOtaAck(src_mac, "chunk_retry", expected_chunk);
      return;
    }
    
    int hex_len = strlen(hex_data);
    int data_len = hex_len / 2;
    uint8_t* raw_buf = (uint8_t*)malloc(data_len);
    if (raw_buf) {
      for (int i = 0; i < data_len; i++) {
        raw_buf[i] = hexToByte(hex_data[i * 2], hex_data[i * 2 + 1]);
      }
      otaWriteFile.write(raw_buf, data_len);
      free(raw_buf);
      
      sendOtaAck(src_mac, "chunk_ok", chunk_id);
      expected_chunk++;
      
      if (expected_chunk == otaMeta.total_chunks) {
        otaWriteFile.close();
        ota_in_progress = false;
        
        String tmpPath = "/" + String(otaMeta.name) + ".tflite.tmp";
        String targetPath = "/" + String(otaMeta.name) + ".tflite";
        
        // 验证临时文件大小
        File checkFile = SD.open(tmpPath);
        if (checkFile) {
          size_t actualSize = checkFile.size();
          checkFile.close();
          Serial.printf("[OTA] Temp file size: %zu bytes (expected %d)\n", actualSize, otaMeta.total_size);
          if (actualSize != (size_t)otaMeta.total_size) {
            Serial.printf("[OTA] SIZE MISMATCH! Expected %d, got %zu\n", otaMeta.total_size, actualSize);
            SD.remove(tmpPath);
            sendOtaAck(src_mac, "model_fail", 0);
            return;
          }
        } else {
          Serial.println("[OTA] Cannot open temp file for size check!");
        }
        
        if (SD.exists(targetPath.c_str())) SD.remove(targetPath.c_str());
        
        if (SD.rename(tmpPath.c_str(), targetPath.c_str())) {
          Serial.println("[OTA] Rename successful. Reloading TF Model...");
          model_loaded = false;
          if (loadModelByFile(targetPath.c_str())) {
            model_loaded = true;
            scanModels();
            sendOtaAck(src_mac, "model_done", 0);
          } else {
            Serial.println("[OTA] loadModelByFile FAILED");
            sendOtaAck(src_mac, "model_fail", 0);
          }
        } else {
          Serial.println("[OTA] Rename FAILED");
          sendOtaAck(src_mac, "model_fail", 0);
        }
      }
    } else {
      sendOtaAck(src_mac, "model_fail", 0);
    }
    return;
  }

  // 3. 处理消杀指令
  if (strcmp(packet, "uv_on") == 0) {
    uvState = true;
    digitalWrite(RELAY_PIN, HIGH);
    uvStartTime = millis();
    Serial.println("[UV] Forced ON");
  } 
  else if (strcmp(packet, "uv_off") == 0) {
    uvState = false;
    digitalWrite(RELAY_PIN, LOW);
    uvStartTime = 0;
    Serial.println("[UV] Forced OFF");
  }
  else if (strcmp(packet, "uv_auto_on") == 0) {
    uvAutoMode = true;
    Serial.println("[UV] Auto mode enabled");
  }
  else if (strcmp(packet, "uv_auto_off") == 0) {
    uvAutoMode = false;
    Serial.println("[UV] Auto mode disabled");
  }
  else if (strncmp(packet, "uv_dur_", 7) == 0) {
    int sec = atoi(packet + 7);
    uvOnDuration = (unsigned long)sec * 1000;
    Serial.printf("[UV] Duration set to %d seconds\n", sec);
  }
  else if (strncmp(packet, "uv_thresh_", 10) == 0) {
    uvThreshold = atoi(packet + 10);
    Serial.printf("[UV] Threshold set to %d\n", uvThreshold);
  }
  // 4. 处理雾化加湿器指令
  else if (strcmp(packet, "fog_on") == 0) {
    fogState = true;
    digitalWrite(FOG_PIN, HIGH);
    fogStartTime = millis();
    Serial.println("[FOG] Forced ON");
  }
  else if (strcmp(packet, "fog_off") == 0) {
    fogState = false;
    digitalWrite(FOG_PIN, LOW);
    fogStartTime = 0;
    Serial.println("[FOG] Forced OFF");
  }
  else if (strcmp(packet, "fog_auto_on") == 0) {
    fogAutoMode = true;
    Serial.println("[FOG] Auto mode enabled");
  }
  else if (strcmp(packet, "fog_auto_off") == 0) {
    fogAutoMode = false;
    Serial.println("[FOG] Auto mode disabled");
  }
  else if (strncmp(packet, "fog_dur_", 8) == 0) {
    int sec = atoi(packet + 8);
    fogOnDuration = (unsigned long)sec * 1000;
    Serial.printf("[FOG] Duration set to %d seconds\n", sec);
  }
  else if (strcmp(packet, "fog_status") == 0) {
    sendDeviceStatus(src_mac, "fog", fogState, fogAutoMode, fogOnDuration, fogStartTime);
  }
  // 5. 处理风扇指令
  else if (strcmp(packet, "fan_on") == 0) {
    fanState = true;
    digitalWrite(FAN_PIN, HIGH);
    fanStartTime = millis();
    Serial.println("[FAN] Forced ON");
  }
  else if (strcmp(packet, "fan_off") == 0) {
    fanState = false;
    digitalWrite(FAN_PIN, LOW);
    fanStartTime = 0;
    Serial.println("[FAN] Forced OFF");
  }
  else if (strcmp(packet, "fan_auto_on") == 0) {
    fanAutoMode = true;
    Serial.println("[FAN] Auto mode enabled");
  }
  else if (strcmp(packet, "fan_auto_off") == 0) {
    fanAutoMode = false;
    Serial.println("[FAN] Auto mode disabled");
  }
  else if (strncmp(packet, "fan_dur_", 8) == 0) {
    int sec = atoi(packet + 8);
    fanOnDuration = (unsigned long)sec * 1000;
    Serial.printf("[FAN] Duration set to %d seconds\n", sec);
  }
  else if (strcmp(packet, "fan_status") == 0) {
    sendDeviceStatus(src_mac, "fan", fanState, fanAutoMode, fanOnDuration, fanStartTime);
  }
}


// ==================== 函数声明 ====================
// 前向声明 - 模型管理
bool loadWiFiConfig();
void scanModels();
void scanDir(const char* path);
void listModels();
bool loadModelByIndex(int index);
bool loadModelByFile(const char* filename);
bool loadModelFromSD();
bool saveModelToSD(uint8_t* data, size_t size);
void loadNormParams(const char* modelPath);

// 前向声明 - 类别管理
void initDefaultClasses();
bool loadClassesFromJson(const char* jsonPath);

// 前向声明 - 推理相关
void updateBaseline(const SensorData &data);
void resetBaseline();
void preprocessInput(const SensorData &data, float input[10]);
int runInference(const SensorData &data);
float getConfidence();
int getFoodType(int predictedClass, const char* className);
int getFreshnessState(const char* className);
int calculateFreshnessScore(float confidence, int predictedClass);

// 前向声明 - 数据记录
void logSensorDataToSD(const SensorData &data, int pred_class, float freshness);

// 前向声明 - ESP-NOW
void addPeer(const uint8_t *mac, const char *name);
void onReceive(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len);
void checkAllConnections();

// 前向声明 - 串口任务
void serialTask(void *arg);

// 前向声明 - WiFi功能
void startWiFiUpdateMode();
void stopWiFiUpdateMode();
void handleRoot();
void handleUpload();
void handleStatus();
void handleNotFound();
void handleRestart();
void handleModelsList();
void handleModelLoad();
String getHTMLPage();
String getStatusJSON();
int calculateFreshnessScore(float confidence, int predictedClass);
void logSensorDataToSD(const SensorData &data, int pred_class, float freshness);

// ==================== setup ====================
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("\n===== ESP-NOW Receiver - Food Freshness =====");
  
  // PSRAM 初始化 - 外挂 OPI PSRAM 支持
  Serial.println("Initializing PSRAM...");
  
  // 尝试初始化 PSRAM（OPI 模式用于外挂 8MB PSRAM）
  bool psramOk = false;
  
  #if CONFIG_IDF_TARGET_ESP32S3
    // ESP32-S3 外挂 OPI PSRAM 初始化
    psramOk = psramInit();
    if (!psramOk) {
      Serial.println("psramInit() failed, trying esp_psram_init()...");
      psramOk = (esp_psram_init() == ESP_OK);
    }
  #else
    psramOk = psramInit();
  #endif
  
  if (psramOk) {
    size_t psramSize = ESP.getPsramSize();
    size_t freePsram = ESP.getFreePsram();
    Serial.printf("PSRAM: %d bytes total\n", psramSize);
    Serial.printf("Free PSRAM: %d bytes\n", freePsram);
    if (psramSize == 0) {
      Serial.println("WARNING: PSRAM init OK but size=0, check board settings!");
    }
  } else {
    Serial.println("PSRAM: initialization FAILED!");
    Serial.println("-> Check Tools > PSRAM setting in Arduino IDE");
    Serial.println("-> For external PSRAM, select 'OPI PSRAM'");
  }
  
  startTime = millis();
  
  // 初始化默认类别（后续会从SD卡加载实际的）
  initDefaultClasses();

  // SD卡初始化（失败不影响采集）
  if (initSD()) {
    Serial.println("SD OK");

    // 加载WiFi配置
    loadWiFiConfig();

    bool testDirExists = SD.exists("/test");
    if (!testDirExists) SD.mkdir("/test");

    // 扫描SD卡上的所有模型
    scanModels();
    listModels();

    // 默认加载 model.tflite 或第一个模型
    if (modelCount > 0) {
      if (!loadModelByIndex(0)) {
        Serial.println("Model load failed");
      } else {
        model_loaded = true;
      }
    } else {
      Serial.println("No model found on SD");
    }

    char filename[64];
    time_t unixTime = time(NULL);
    if (unixTime > 1000000000) {
      struct tm *tm;
      tm = gmtime(&unixTime);
      snprintf(filename, sizeof(filename), "/test/sensor_log_%04d%02d%02d_%02d%02d.csv", 
        tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday, tm->tm_hour, tm->tm_min);
    } else {
      String mac = WiFi.macAddress();
      mac.replace(":", "");
      snprintf(filename, sizeof(filename), "/test/sensor_%s_%lu.csv", mac.c_str(), (unsigned long)millis());
    }
    
    data_file = SD.open(filename, FILE_WRITE);
    if (data_file) {
      data_file.println("timestamp,odor,hcho,co,voc,co2,co2_temp,env_temp,humidity,status,pred,freshness");
      data_file.flush();
    }
  } else {
    Serial.println("[WARN] SD card failed - data collection only (no logging, no model)");
  }

  // 紫外灯继电器初始化
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // 雾化和风扇引脚初始化
  pinMode(FOG_PIN, OUTPUT);
  digitalWrite(FOG_PIN, LOW);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);


  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (1) delay(100);
  }
  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, sensorMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  addPeer(sensorMac, "Sender");
  xTaskCreatePinnedToCore(serialTask, "serialTask", 4096, NULL, 1, NULL, 1);

  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println();
  Serial.println("===== Commands =====");
  Serial.println("wifi       - Start WiFi update mode");
  Serial.println("stop       - Stop WiFi update mode");
  Serial.println("info       - Show model info");
  Serial.println("model list - List all models on SD");
  Serial.println("model load N - Load model #N");
  Serial.println("classes    - Show loaded class names");
  Serial.println("calibrate  - Reset air baseline");
  Serial.println("status     - System status");
  Serial.println("===================");
  Serial.println("Waiting for data or commands...");
}

// ==================== SD卡初始化 ====================
bool initSD() {
  sd_spi = new SPIClass();
  sd_spi->begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  if (!SD.begin(SD_CS, *sd_spi)) {
    sd_ready = false;
    return false;
  }
  sd_ready = true;
  return true;
}

// ==================== 从SD卡读取WiFi配置 ====================
bool loadWiFiConfig() {
  if (!sd_ready) return false;
  File f = SD.open(WIFI_CONF_FILE);
  if (!f) {
    Serial.println("[WiFi] No wifi.conf on SD, WiFi update disabled");
    return false;
  }
  String line1 = f.readStringUntil('\n');
  String line2 = f.readStringUntil('\n');
  f.close();
  line1.trim();
  line2.trim();
  if (line1.length() == 0 || line2.length() == 0) {
    Serial.println("[WiFi] wifi.conf format: line1=SSID, line2=PASSWORD");
    return false;
  }
  strncpy(wifi_ssid, line1.c_str(), sizeof(wifi_ssid) - 1);
  strncpy(wifi_password, line2.c_str(), sizeof(wifi_password) - 1);
  Serial.printf("[WiFi] Config loaded: SSID=%s\n", wifi_ssid);
  return true;
}

// ==================== 多模型管理 ====================

// 判断字符串是否以指定后缀结尾
bool endsWith(const char* str, const char* suffix) {
  int slen = strlen(str);
  int xlen = strlen(suffix);
  if (xlen > slen) return false;
  return strcmp(str + slen - xlen, suffix) == 0;
}

// 扫描SD卡上的所有 .tflite 模型文件
void scanModels() {
  modelCount = 0;
  
  File root = SD.open("/");
  if (!root) {
    Serial.println("[SCAN] Cannot open SD root");
    return;
  }
  
  Serial.println("[SCAN] Scanning root directory...");
  
  // 枚举根目录下所有文件
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    
    if (entry.isDirectory()) continue;
    
    // entry.name() 在不同 ESP32 Arduino Core 版本返回格式不同:
    // - v2.x: "model.tflite" (无前导/)
    // - v3.x: "/model.tflite" (有前导/)
    // 统一处理
    String nameStr = String(entry.name());
    nameStr.trim();
    
    // 调试: 打印每个找到的文件
    Serial.printf("[SCAN] File: \"%s\"  size=%zu\n", nameStr.c_str(), entry.size());
    
    // 检查是否是 .tflite 文件
    if (!endsWith(nameStr.c_str(), ".tflite")) {
      entry.close();
      continue;
    }
    
    if (modelCount >= MAX_MODELS) {
      entry.close();
      break;
    }
    
    // 构造完整的绝对路径 (确保以 / 开头)
    String fullPath;
    if (nameStr.startsWith("/")) {
      fullPath = nameStr;
    } else {
      fullPath = "/" + nameStr;
    }
    
    // 提取标签 (去掉路径和后缀)
    // "/model.tflite" -> "model"
    // "/food_v2.tflite" -> "food_v2"
    int lastSlash = fullPath.lastIndexOf('/');
    int lastDot = fullPath.lastIndexOf('.');
    String label = fullPath.substring(lastSlash + 1, lastDot);
    if (label.length() == 0) label = fullPath; // fallback
    
    // 保存到模型列表
    strncpy(modelList[modelCount].filename, fullPath.c_str(), 31);
    modelList[modelCount].filename[31] = '\0';
    strncpy(modelList[modelCount].label, label.c_str(), 31);
    modelList[modelCount].label[31] = '\0';
    modelList[modelCount].filesize = entry.size();
    
    Serial.printf("[SCAN]   -> Model[%d]: %s  label=%s  %zu bytes\n", 
      modelCount, modelList[modelCount].filename, modelList[modelCount].label, entry.size());
    
    modelCount++;
    entry.close();
  }
  root.close();
  
  // 额外检查: 如果扫描没找到, 直接用 SD.exists 检查默认模型
  if (modelCount == 0 && SD.exists("/model.tflite")) {
    Serial.println("[SCAN] scanModels found nothing, but /model.tflite exists! Adding manually.");
    File f = SD.open("/model.tflite");
    if (f) {
      strncpy(modelList[0].filename, "/model.tflite", 31);
      strncpy(modelList[0].label, "model", 31);
      modelList[0].filesize = f.size();
      f.close();
      modelCount = 1;
    }
  }
  
  Serial.printf("[SCAN] Total: %d model(s)\n", modelCount);
}

void scanDir(const char* path) {
  // 不再使用递归扫描, 简化为只扫描根目录
  // 如需子目录支持, 在 scanModels 中添加
  scanModels();
}

// 列出所有模型
void listModels() {
  Serial.println("\n===== Models on SD =====");
  for (int i = 0; i < modelCount; i++) {
    Serial.printf("  [%d] %s  (%zu bytes)%s\n", 
      i, 
      modelList[i].filename, 
      modelList[i].filesize,
      (i == currentModelIndex) ? " <- ACTIVE" : "");
  }
  if (modelCount == 0) {
    Serial.println("  (no models found)");
  }
  Serial.println("========================");
}

// 按索引加载模型
bool loadModelByIndex(int index) {
  if (index < 0 || index >= modelCount) {
    Serial.printf("Invalid model index: %d (0-%d)\n", index, modelCount - 1);
    return false;
  }
  return loadModelByFile(modelList[index].filename);
}

// 按文件名加载模型（核心函数）
bool loadModelByFile(const char* filename) {
  if (!sd_ready) return false;

  Serial.printf("Loading model: %s\n", filename);
  
  File file = SD.open(filename);
  if (!file) {
    Serial.println("Cannot open model file");
    return false;
  }

  size_t modelSize = file.size();
  Serial.printf("Model size: %zu bytes\n", modelSize);

  // 释放之前的 interpreter 和模型缓冲区（关键：避免内存泄漏）
  if (interpreter != nullptr) {
    delete interpreter;
    interpreter = nullptr;
    Serial.println("Old interpreter freed");
  }
  if (model_buffer != nullptr) {
    free(model_buffer);
    model_buffer = nullptr;
    Serial.println("Old model buffer freed");
  }

  // 尝试从 PSRAM 分配内存，失败则回退到内部 RAM
  Serial.printf("PSRAM free: %d bytes, need: %zu bytes\n", ESP.getFreePsram(), modelSize);
  model_buffer = (uint8_t*)ps_malloc(modelSize);
  if (!model_buffer) {
    Serial.println("PSRAM alloc failed, trying internal RAM...");
    Serial.printf("Internal free: %d bytes\n", ESP.getFreeHeap());
    model_buffer = (uint8_t*)malloc(modelSize);
  }
  if (!model_buffer) {
    Serial.println("malloc failed");
    file.close();
    return false;
  }
  Serial.printf("Model buffer allocated: %zu bytes\n", modelSize);
  
  size_t bytesRead = file.read(model_buffer, modelSize);
  file.close();
  if (bytesRead != modelSize) {
    Serial.println("Read incomplete");
    free(model_buffer);
    model_buffer = nullptr;
    return false;
  }

  // 验证 TFLite 魔数 (4字节: 0x1A 0x2A 0x00 0x00 或 'TFL3')
  if (bytesRead < 8 || memcmp(model_buffer, "TFL3", 4) != 0) {
    Serial.printf("Invalid TFLite magic bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
      model_buffer[0], model_buffer[1], model_buffer[2], model_buffer[3],
      model_buffer[4], model_buffer[5], model_buffer[6], model_buffer[7]);
    free(model_buffer);
    model_buffer = nullptr;
    return false;
  }
  Serial.println("TFLite magic OK");

  tflite_model = GetModel(model_buffer);
  if (tflite_model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("Schema version mismatch");
    free(model_buffer);
    model_buffer = nullptr;
    tflite_model = nullptr;
    return false;
  }

  // 每次创建新的 interpreter，确保使用新模型（修复 static 无法切换的 bug）
  interpreter = new MicroInterpreter(
    tflite_model, resolver, tensor_arena, TENSOR_ARENA_SIZE, &micro_error_reporter);

  TfLiteStatus status = interpreter->AllocateTensors();
  if (status != kTfLiteOk) {
    Serial.println("AllocateTensors failed");
    delete interpreter;
    interpreter = nullptr;
    free(model_buffer);
    model_buffer = nullptr;
    tflite_model = nullptr;
    return false;
  }

  // 更新当前模型信息
  strncpy(currentModelFile, filename, 31);
  currentModelFile[31] = '\0';
  // 更新当前索引
  for (int i = 0; i < modelCount; i++) {
    if (strcmp(modelList[i].filename, filename) == 0) {
      currentModelIndex = i;
      break;
    }
  }

  Serial.printf("Model loaded OK: %s\n", filename);
  
  // 打印输入输出张量维度详情
  TfLiteTensor* input_tensor = interpreter->input(0);
  TfLiteTensor* output_tensor = interpreter->output(0);
  
  Serial.printf("  Input dims: [");
  for (int i = 0; i < input_tensor->dims->size; i++) {
    Serial.printf("%d", input_tensor->dims->data[i]);
    if (i < input_tensor->dims->size - 1) Serial.printf(", ");
  }
  Serial.printf("]\n");
  
  Serial.printf("  Output dims: [");
  for (int i = 0; i < output_tensor->dims->size; i++) {
    Serial.printf("%d", output_tensor->dims->data[i]);
    if (i < output_tensor->dims->size - 1) Serial.printf(", ");
  }
  Serial.printf("]\n");
  
  Serial.printf("  Input size: %d\n", input_tensor->dims->data[1]);
  Serial.printf("  Output classes: %d\n", output_tensor->dims->data[output_tensor->dims->size - 1]);
  Serial.printf("  Free heap: %d bytes\n", ESP.getFreeHeap());

  // 加载对应的归一化参数（模型和 _norm.json 必须同名）
  loadNormParams(filename);

  return true;
}

// ==================== 加载模型（兼容旧接口） ====================
bool loadModelFromSD() {
  return loadModelByFile(MODEL_FILE);
}

// ==================== 保存模型到SD卡 ====================
bool saveModelToSD(uint8_t* data, size_t size) {
  if (!sd_ready) return false;

  // 删除旧文件
  if (SD.exists(MODEL_FILE)) {
    SD.remove(MODEL_FILE);
    Serial.println("Old model removed");
  }

  // 创建新文件
  File file = SD.open(MODEL_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to create model file");
    return false;
  }

  size_t written = file.write(data, size);
  file.close();

  if (written != size) {
    Serial.println("Model write incomplete");
    SD.remove(MODEL_FILE);
    return false;
  }

  Serial.printf("Model saved: %zu bytes\n", size);
  return true;
}

// ==================== 记录数据 ====================
void logSensorDataToSD(const SensorData &data, int pred_class, float freshness) {
  if (!sd_ready || !data_file) return;
  data_file.printf("%u,%.2f,%.2f,%.2f,%.2f,%u,%d,%.2f,%.2f,%u,%d,%.2f\n",
    data.timestamp, data.odor_ppm, data.hcho_ppm, data.co_ppm, data.voc_ppm,
    data.co2_ppm, data.co2_temp, data.env_temp, data.humidity,
    data.sensor_status, pred_class, freshness);
  if (data_file.position() > 4096) data_file.flush();
}

// ==================== 串口任务 ====================
void serialTask(void *arg) {
  while (true) {
    if (Serial.available() > 0) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      
      if (cmd.equalsIgnoreCase("wifi")) {
        Serial.println("Starting WiFi update mode...");
        startWiFiUpdateMode();
      } 
      else if (cmd.equalsIgnoreCase("stop")) {
        Serial.println("Stopping WiFi update mode...");
        stopWiFiUpdateMode();
      }
      else if (cmd.equalsIgnoreCase("info")) {
        Serial.println("\n===== Model Info =====");
        if (model_loaded && interpreter) {
          TfLiteTensor* input_tensor = interpreter->input(0);
          TfLiteTensor* output_tensor = interpreter->output(0);
          Serial.printf("Model: %s\n", currentModelFile);
          Serial.printf("Input dims: [");
          for (int i = 0; i < input_tensor->dims->size; i++) {
            Serial.printf("%d", input_tensor->dims->data[i]);
            if (i < input_tensor->dims->size - 1) Serial.printf(", ");
          }
          Serial.printf("]\n");
          Serial.printf("Output dims: [");
          for (int i = 0; i < output_tensor->dims->size; i++) {
            Serial.printf("%d", output_tensor->dims->data[i]);
            if (i < output_tensor->dims->size - 1) Serial.printf(", ");
          }
          Serial.printf("]\n");
          Serial.printf("Classes: %d\n", numClasses);
          Serial.printf("Norm params: %s\n", normParams.loaded ? "Loaded" : "Not loaded");
        } else {
          Serial.println("No model loaded");
        }
        Serial.println("======================");
      }
      else if (cmd.equalsIgnoreCase("model list") || cmd.equalsIgnoreCase("models")) {
        scanModels();
        listModels();
      }
      else if (cmd.startsWith("model load ")) {
        int idx = cmd.substring(11).toInt();
        Serial.printf("Loading model #%d...\n", idx);
        model_loaded = false;
        if (loadModelByIndex(idx)) {
          model_loaded = true;
          Serial.printf("Switched to model [%d]: %s\n", idx, modelList[idx].filename);
        }
      }
      else if (cmd.equalsIgnoreCase("model rescan")) {
        scanModels();
        listModels();
      }
      else if (cmd.equalsIgnoreCase("classes")) {
        Serial.printf("\n===== Classes (%d) =====\n", numClasses);
        for (int i = 0; i < numClasses; i++) {
          Serial.printf("  [%d] %s\n", i, classNames[i]);
        }
        Serial.println("======================");
      }
      else if (cmd.equalsIgnoreCase("calibrate")) {
        resetBaseline();
        Serial.println("Baseline reset, recalibrating from next data...");
      }
      else if (cmd.equalsIgnoreCase("status")) {
        Serial.println("\n===== Status =====");
        Serial.printf("WiFi Mode: %s\n", wifi_update_mode ? "ON" : "OFF");
        if (wifi_update_mode) {
          Serial.printf("IP Address: http://%s\n", WiFi.localIP().toString().c_str());
        }
        Serial.printf("Model Loaded: %s\n", model_loaded ? "Yes" : "No");
        if (model_loaded) {
          Serial.printf("Current Model: [%d] %s\n", currentModelIndex, currentModelFile);
        }
        Serial.printf("Models on SD: %d\n", modelCount);
        Serial.printf("Model Updated: %s\n", model_updated ? "Yes (restart to reload)" : "No");
        Serial.printf("SD Card: %s\n", sd_ready ? "OK" : "Failed");
        Serial.printf("ESP-NOW: Online\n");
        Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
        Serial.println("===================");
      }
      else if (cmd.length() > 0 && !wifi_update_mode) {
        CommandPacket pkt;
        pkt.dataType = PKT_TYPE_COMMAND;
        strncpy(pkt.command, cmd.c_str(), 31);
        pkt.command[31] = '\0';
        pkt.timestamp = millis();
        esp_now_send(sensorMac, (uint8_t*)&pkt, sizeof(pkt));
      }
    }
    delay(10);
  }
}

// ==================== WiFi更新模式 ====================
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

  // 连接到WiFi
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

  // 配置服务器
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
  WiFi.disconnect();
  wifi_update_mode = false;
  Serial.println("WiFi update mode stopped");
}

// ==================== HTTP处理函数 ====================
void handleRoot() {
  server.send(200, "text/html", getHTMLPage());
}

void handleStatus() {
  server.send(200, "application/json", getStatusJSON());
}

void handleUpload() {
  Serial.println("[WiFi] Upload request received");
  
  // 获取目标文件名（可选，默认 model.tflite）
  String targetFile = "/model.tflite";
  if (server.hasArg("name")) {
    String name = server.arg("name");
    if (name.length() > 0) {
      // 确保以 / 开头且以 .tflite 结尾
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
    size_t maxModelSize = 500000;  // 500KB上限（ESP32-S3可用堆约300KB，模型+arena需适配）
    for (size_t i = 0; i + 1 < modelData.length() && dataSize < maxModelSize; i += 2) {
      char hex[3] = {modelData.charAt(i), modelData.charAt(i+1), 0};
      buffer[dataSize++] = strtol(hex, NULL, 16);
    }
    
    Serial.printf("[WiFi] Decoded: %d bytes\n", dataSize);
    
    // 保存到指定文件
    if (SD.exists(targetFile.c_str())) {
      SD.remove(targetFile.c_str());
    }
    File file = SD.open(targetFile.c_str(), FILE_WRITE);
    if (file) {
      size_t written = file.write(buffer, dataSize);
      file.close();
      if (written == dataSize) {
        Serial.printf("[WiFi] Model saved: %s (%zu bytes)\n", targetFile.c_str(), dataSize);
        // 重新扫描模型列表
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

// 模型列表 API
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

// 模型加载 API
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

// ==================== HTML页面 ====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
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

String getHTMLPage() {
  char buf[1024];
  strcpy_P(buf, INDEX_HTML);
  return String(buf);
}

String getStatusJSON() {
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
  return json;
}

// ==================== loop ====================
void loop() {
  checkAllConnections();
  
  if (wifi_update_mode) {
    server.handleClient();
  }
  
  // 紫外消杀定时自动关闭逻辑
  if (uvState && uvStartTime > 0) {
    if (millis() - uvStartTime >= uvOnDuration) {
      uvState = false;
      digitalWrite(RELAY_PIN, LOW);
      uvStartTime = 0;
      Serial.println("[UV] Timer expired, turning OFF");
    }
  }

  // 雾化定时自动关闭逻辑
  if (fogState && fogStartTime > 0) {
    if (millis() - fogStartTime >= fogOnDuration) {
      fogState = false;
      digitalWrite(FOG_PIN, LOW);
      fogStartTime = 0;
      Serial.println("[FOG] Timer expired, turning OFF");
    }
  }

  // 风扇定时自动关闭逻辑
  if (fanState && fanStartTime > 0) {
    if (millis() - fanStartTime >= fanOnDuration) {
      fanState = false;
      digitalWrite(FAN_PIN, LOW);
      fanStartTime = 0;
      Serial.println("[FAN] Timer expired, turning OFF");
    }
  }

  
  delay(10);
}

// ==================== ESP-NOW接收 ====================
void onReceive(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  for (int i = 0; i < peerCount; i++) {
    if (memcmp(info->src_addr, peers[i].mac, 6) == 0) {
      peers[i].lastSeen = millis();
      break;
    }
  }

  // 1. 优先根据第一个字节或长度匹配结构体类型
  if (len == sizeof(SensorData) && incomingData[0] == PKT_TYPE_SENSOR) {
    SensorData tmp;
    memcpy(&tmp, incomingData, sizeof(tmp));
    latestData = tmp;
    hasValidSensorData = true;
    
    // 输出GUI采集格式
    Serial.printf("$DATA:%.2f,%.2f,%.2f,%.2f,%u,%u\n", 
      tmp.odor_ppm, tmp.hcho_ppm, tmp.co_ppm, tmp.voc_ppm, tmp.co2_ppm, tmp.timestamp);
    
    int predClass = 0;
    float conf = 0;
    int freshness = 0;

    if (model_loaded) {
      predClass = runInference(latestData);
      conf = getConfidence();
      freshness = calculateFreshnessScore(conf, predClass);
      
      // 紫外消杀智能自动消杀判定
      if (uvAutoMode) {
        if (predClass != 0 && freshness < uvThreshold) {
          if (!uvState) {
            uvState = true;
            digitalWrite(RELAY_PIN, HIGH);
            uvStartTime = millis();
            Serial.printf("[UV] Auto-triggered ON (freshness=%d < threshold=%d)\n", freshness, uvThreshold);
          }
        }
      }

      // 自动加湿智能判定 (湿度 < 40% 触发开启，湿度 >= 40% 关闭)
      if (fogAutoMode) {
        if (tmp.humidity < 40.0f) {
          if (!fogState) {
            fogState = true;
            digitalWrite(FOG_PIN, HIGH);
            fogStartTime = millis();
            Serial.printf("[FOG] Auto-triggered ON (humidity=%.1f < 40.0)\n", tmp.humidity);
          }
        } else {
          if (fogState) {
            fogState = false;
            digitalWrite(FOG_PIN, LOW);
            fogStartTime = 0;
            Serial.printf("[FOG] Auto-triggered OFF (humidity=%.1f >= 40.0)\n", tmp.humidity);
          }
        }
      }

      // 自动排风智能判定 (新鲜度 <= 50 触发开启，新鲜度 > 50 关闭)
      if (fanAutoMode) {
        if (freshness <= 50) {
          if (!fanState) {
            fanState = true;
            digitalWrite(FAN_PIN, HIGH);
            fanStartTime = millis();
            Serial.printf("[FAN] Auto-triggered ON (freshness=%d <= 50)\n", freshness);
          }
        } else {
          if (fanState) {
            fanState = false;
            digitalWrite(FAN_PIN, LOW);
            fanStartTime = 0;
            Serial.printf("[FAN] Auto-triggered OFF (freshness=%d > 50)\n", freshness);
          }
        }
      }


      // 解析种类和状态
      int foodType = getFoodType(predClass, classNames[predClass]);
      int freshState = getFreshnessState(classNames[predClass]);
      const char* typeNames[] = {"Air", "Apple", "Banana", "Chocolate", "General"};
      const char* stateNames[] = {"Fresh", "Stale", "Rotten"};
      
      if (baselineValid) {
        // 基准就绪后显示完整信息（两步判断结果）
        if (predClass == 0) {
          Serial.printf("-> [Air] fresh=50/100 | Delta: Odor=%+.1f HCHO=%+.1f CO=%+.1f VOC=%+.1f CO2=%+.0f\n",
            tmp.odor_ppm - airBaseline[0],
            tmp.hcho_ppm - airBaseline[1],
            tmp.co_ppm   - airBaseline[2],
            tmp.voc_ppm  - airBaseline[3],
            (float)tmp.co2_ppm - airBaseline[4]);
        } else {
          Serial.printf("-> Type:%s State:%s Conf:%.1f%% Fresh:%d/100 | Delta: O=%+.1f H=%+.1f C=%+.1f V=%+.1f CO2=%+.0f\n",
            typeNames[foodType], stateNames[freshState], conf * 100, freshness,
            tmp.odor_ppm - airBaseline[0],
            tmp.hcho_ppm - airBaseline[1],
            tmp.co_ppm   - airBaseline[2],
            tmp.voc_ppm  - airBaseline[3],
            (float)tmp.co2_ppm - airBaseline[4]);
        }
      } else {
        // 校准中
        Serial.printf("[CALIB] %d/%d samples... baseline: %.1f,%.1f,%.1f,%.1f,%.0f\n",
          baselineCount, BASELINE_SAMPLES,
          airBaseline[0], airBaseline[1], airBaseline[2], airBaseline[3], airBaseline[4]);
      }
      logSensorDataToSD(tmp, predClass, freshness);
    }
  }
  else if (len == sizeof(WarmupStatus) && incomingData[0] == PKT_TYPE_WARMUP) {
    WarmupStatus warmup;
    memcpy(&warmup, incomingData, sizeof(warmup));
    Serial.printf("\n[Warmup] Remaining: %d sec\n", warmup.remainingSec);
  }
  // 2. 对于不匹配上面结构体长度的非零包，拦截并解析为字符串类型命令包
  else if (len > 0 && len < 250) {
    char* packet = (char*)malloc(len + 1);
    if (packet) {
      memcpy(packet, incomingData, len);
      packet[len] = '\0';
      
      // 执行 OTA 与消杀指令解析
      handleCommandPacket(packet, info->src_addr);
      
      free(packet);
    }
  }
}

// ==================== 连接管理 ====================
void addPeer(const uint8_t *mac, const char *name) {
  if (peerCount >= 4) return;
  memcpy(peers[peerCount].mac, mac, 6);
  peers[peerCount].lastSeen = 0;
  peers[peerCount].wasConnected = false;
  strncpy(peers[peerCount].name, name, 15);
  peerCount++;
}

void checkAllConnections() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if (now - lastCheck < 1000) return;
  lastCheck = now;

  bool inGrace = (now - startTime) < STARTUP_GRACE;
  static int offlineCnt[4] = {0};

  for (int i = 0; i < peerCount; i++) {
    bool online = (now - peers[i].lastSeen) <= CONNECTION_TIMEOUT;
    if (inGrace) online = true;

    if (!online && peers[i].wasConnected) {
      offlineCnt[i]++;
      if (offlineCnt[i] >= OFFLINE_CONFIRM_COUNT) {
        Serial.printf("\n[Offline] %s\n", peers[i].name);
        peers[i].wasConnected = false;
        offlineCnt[i] = 0;
      }
    } else if (online && !peers[i].wasConnected) {
      offlineCnt[i] = 0;
      Serial.printf("\n[Online] %s\n", peers[i].name);
      peers[i].wasConnected = true;
    }
  }
}

// ==================== 帮助函数 ====================
String getStatusString(uint8_t status) {
  String s = "";
  if (status & STATUS_ADS1115_OK) s += "ADS1115 ";
  if (status & STATUS_MHZ19C_OK) s += "MHZ19C ";
  if (status & STATUS_BME680_OK) s += "BME680 ";
  if (s.length() == 0) s = "None";
  return s;
}

// ==================== 归一化参数 ====================
// 从训练工具导出的 _norm.json 读取，同步训练时的归一化参数
NormParams normParams = {{0}, {0}, false};

// 解析 JSON 中的 min/max 数组（极简实现，不依赖外部库）
// 格式: {"min":[x,...x],"max":[x,...x]} (10个值)
void loadNormParams(const char* modelPath) {
  normParams.loaded = false;

  // 构造同名 .json 文件路径（支持两种命名：model_norm.json 或 norm.json）
  char jsonPath[64];
  strncpy(jsonPath, modelPath, 63);
  jsonPath[63] = '\0';
  
  // 尝试两种命名方式
  char* dot = strstr(jsonPath, ".tflite");
  if (!dot) return;
  
  // 先尝试 model_norm.json
  strcpy(dot, "_norm.json");
  if (!SD.exists(jsonPath)) {
    // 再尝试 norm.json（根目录下的通用命名）
    strcpy(jsonPath, "/norm.json");
    if (!SD.exists(jsonPath)) {
      Serial.println("[NORM] No norm file found (tried model_norm.json, norm.json)");
      // 即使没有 norm 文件，也尝试加载类别
      loadClassesFromJson("/norm.json");
      return;
    }
  }

  File f = SD.open(jsonPath);
  if (!f) return;

  String content = "";
  while (f.available()) content += (char)f.read();
  f.close();

  // 解析 min 数组 (10个)
  int p = content.indexOf("\"min\"");
  if (p < 0) return;
  for (int i = 0; i < 10; i++) {
    int colon = content.indexOf(":", p);
    int comma = content.indexOf(",", colon);
    int close  = content.indexOf("]", colon);
    if (comma < 0) comma = close;
    int pick = (comma < close) ? comma : close;
    String val = content.substring(colon + 1, pick);
    val.trim();
    normParams.min[i] = val.toFloat();
    p = comma;
  }

  // 解析 max 数组 (10个)
  p = content.indexOf("\"max\"");
  if (p < 0) return;
  for (int i = 0; i < 10; i++) {
    int colon = content.indexOf(":", p);
    int comma = content.indexOf(",", colon);
    int close  = content.indexOf("]", colon);
    if (comma < 0) comma = close;
    int pick = (comma < close) ? comma : close;
    String val = content.substring(colon + 1, pick);
    val.trim();
    normParams.max[i] = val.toFloat();
    p = comma;
  }

  normParams.loaded = true;
  Serial.printf("[NORM] Loaded: min[0..4]=%.2f,%.2f,%.2f,%.2f,%.0f\n",
    normParams.min[0], normParams.min[1], normParams.min[2], normParams.min[3], normParams.min[4]);
  Serial.printf("[NORM] Loaded: min[5..9]=%.2f,%.2f,%.2f,%.2f,%.0f\n",
    normParams.min[5], normParams.min[6], normParams.min[7], normParams.min[8], normParams.min[9]);
  Serial.printf("[NORM] Loaded: max[0..4]=%.2f,%.2f,%.2f,%.2f,%.0f\n",
    normParams.max[0], normParams.max[1], normParams.max[2], normParams.max[3], normParams.max[4]);
  Serial.printf("[NORM] Loaded: max[5..9]=%.2f,%.2f,%.2f,%.2f,%.0f\n",
    normParams.max[5], normParams.max[6], normParams.max[7], normParams.max[8], normParams.max[9]);
  
  // 同时加载类别名称
  loadClassesFromJson(jsonPath);
}

// ==================== 动态类别加载 ====================

// 初始化默认类别（程序启动时调用）
void initDefaultClasses() {
  numClasses = defaultNumClasses;
  for (int i = 0; i < numClasses; i++) {
    strncpy(classNamesBuffer[i], defaultClassNames[i], 31);
    classNamesBuffer[i][31] = '\0';
    classNames[i] = classNamesBuffer[i];
  }
  Serial.printf("[CLASSES] Default: %d classes loaded\n", numClasses);
}

// 从 norm.json 解析类别名称
// 格式: {"classes":["air","apple_fresh",...]}
bool loadClassesFromJson(const char* jsonPath) {
  if (!SD.exists(jsonPath)) {
    Serial.printf("[CLASSES] No JSON file: %s, using defaults\n", jsonPath);
    return false;
  }

  File f = SD.open(jsonPath);
  if (!f) {
    Serial.println("[CLASSES] Failed to open JSON");
    return false;
  }

  String content = "";
  while (f.available()) content += (char)f.read();
  f.close();

  // 查找 "classes" 数组
  int classesPos = content.indexOf("\"classes\"");
  if (classesPos < 0) {
    Serial.println("[CLASSES] No 'classes' field in JSON");
    return false;
  }

  // 找到数组开始 [
  int bracketOpen = content.indexOf("[", classesPos);
  if (bracketOpen < 0) {
    Serial.println("[CLASSES] No array start bracket");
    return false;
  }

  // 解析类别名称
  int count = 0;
  int pos = bracketOpen + 1;
  
  while (count < MAX_CLASSES && pos < content.length()) {
    // 查找下一个引号
    int quote1 = content.indexOf("\"", pos);
    if (quote1 < 0) break;
    
    // 查找结束引号
    int quote2 = content.indexOf("\"", quote1 + 1);
    if (quote2 < 0) break;
    
    // 提取类别名称
    String className = content.substring(quote1 + 1, quote2);
    className.trim();
    
    if (className.length() > 0) {
      strncpy(classNamesBuffer[count], className.c_str(), 31);
      classNamesBuffer[count][31] = '\0';
      classNames[count] = classNamesBuffer[count];
      count++;
    }
    
    // 移动到下一个
    pos = quote2 + 1;
    
    // 检查是否到达数组结束
    int nextBracket = content.indexOf("]", quote2);
    int nextComma = content.indexOf(",", quote2);
    if (nextBracket > 0 && (nextComma < 0 || nextBracket < nextComma)) {
      break;  // 数组结束
    }
  }

  if (count > 0) {
    numClasses = count;
    Serial.printf("[CLASSES] Loaded %d classes from SD:\n", numClasses);
    for (int i = 0; i < numClasses && i < 5; i++) {
      Serial.printf("  [%d] %s\n", i, classNames[i]);
    }
    if (numClasses > 5) {
      Serial.printf("  ... and %d more\n", numClasses - 5);
    }
    return true;
  }

  Serial.println("[CLASSES] Failed to parse classes, using defaults");
  return false;
}

// ==================== 模型推理 ====================
// 更新空气基准（开机时或重新校准时调用）
void updateBaseline(const SensorData &data) {
  float total_odor = airBaseline[0] * baselineCount;
  float total_hcho = airBaseline[1] * baselineCount;
  float total_co   = airBaseline[2] * baselineCount;
  float total_voc  = airBaseline[3] * baselineCount;
  float total_co2  = airBaseline[4] * baselineCount;
  
  baselineCount++;
  airBaseline[0] = (total_odor + data.odor_ppm) / baselineCount;
  airBaseline[1] = (total_hcho + data.hcho_ppm) / baselineCount;
  airBaseline[2] = (total_co   + data.co_ppm)   / baselineCount;
  airBaseline[3] = (total_voc  + data.voc_ppm)   / baselineCount;
  airBaseline[4] = (total_co2  + (float)data.co2_ppm) / baselineCount;
  
  if (baselineCount >= BASELINE_SAMPLES) {
    baselineValid = true;
    Serial.printf("[CALIB] Baseline ready: Odor=%.1f HCHO=%.1f CO=%.1f VOC=%.1f CO2=%.0f\n",
      airBaseline[0], airBaseline[1], airBaseline[2], airBaseline[3], airBaseline[4]);
  }
}

// 重置基准（重新校准）
void resetBaseline() {
  baselineValid = false;
  baselineCount = 0;
  airBaseline[0] = airBaseline[1] = airBaseline[2] = airBaseline[3] = airBaseline[4] = 0;
  Serial.println("[CALIB] Baseline reset");
}

void preprocessInput(const SensorData &data, float input[10]) {
  // 前5维：空气基准
  input[0] = airBaseline[0];
  input[1] = airBaseline[1];
  input[2] = airBaseline[2];
  input[3] = airBaseline[3];
  input[4] = airBaseline[4];
  
  // 后5维：变化量（当前读数 - 空气基准）
  input[5] = data.odor_ppm - airBaseline[0];   // Odor delta
  input[6] = data.hcho_ppm - airBaseline[1];   // HCHO delta
  input[7] = data.co_ppm   - airBaseline[2];   // CO delta
  input[8] = data.voc_ppm  - airBaseline[3];   // VOC delta
  input[9] = (float)data.co2_ppm - airBaseline[4];  // CO2 delta

  // 应用归一化（MinMaxScaler -> [0, 1]）
  if (normParams.loaded) {
    for (int i = 0; i < 10; i++) {
      float range = normParams.max[i] - normParams.min[i];
      if (range > 0.0f) {
        input[i] = (input[i] - normParams.min[i]) / range;
        if (input[i] < 0.0f) input[i] = 0.0f;
        if (input[i] > 1.0f) input[i] = 1.0f;
      }
    }
  }
}

int runInference(const SensorData &data) {
  if (!interpreter || !model_loaded) {
    last_confidence = 0.0f;
    last_predicted = 0;
    return 0;
  }

  // 更新基准（如果还没稳定）
  if (!baselineValid) {
    updateBaseline(data);
    last_confidence = 0.0f;
    last_predicted = 0;
    return 0;
  }

  float input[10];
  preprocessInput(data, input);
  
  float* input_tensor = interpreter->input(0)->data.f;
  for (int i = 0; i < 10; i++) {
    input_tensor[i] = input[i];
  }
  
  TfLiteStatus invoke_status = interpreter->Invoke();
  if (invoke_status != kTfLiteOk) {
    Serial.println("Invoke failed");
    return 0;
  }
  
  float* output_tensor = interpreter->output(0)->data.f;
  
  // 获取输出维度（支持 [batch, classes] 或 [classes]）
  TfLiteTensor* output = interpreter->output(0);
  int output_dims = output->dims->size;
  int num_classes = output->dims->data[output_dims - 1];  // 最后一维是类别数
  
  int predictedClass = 0;
  float maxProb = 0.0f;
  for (int i = 0; i < num_classes; i++) {
    if (output_tensor[i] > maxProb) {
      maxProb = output_tensor[i];
      predictedClass = i;
    }
  }
  
  last_confidence = maxProb;
  last_predicted = predictedClass;
  return predictedClass;
}

float getConfidence() {
  return last_confidence;
}

// 从类别名称解析食物种类和新鲜度
// 返回: 种类索引 (0=air, 1=apple, 2=banana, 3=chocolate, 4=general)
int getFoodType(int predictedClass, const char* className) {
  if (predictedClass == 0 || strstr(className, "air")) return 0;
  if (strstr(className, "apple")) return 1;
  if (strstr(className, "banana")) return 2;
  if (strstr(className, "chocolate")) return 3;
  return 4; // general
}

// 从类别名称解析新鲜度状态
// 返回: 0=fresh, 1=stale, 2=rotten
int getFreshnessState(const char* className) {
  if (strstr(className, "fresh")) return 0;
  if (strstr(className, "stale")) return 1;
  if (strstr(className, "rotten")) return 2;
  return 0; // 默认fresh
}

// 计算新鲜度分数（基于种类和状态分别判断）
int calculateFreshnessScore(float confidence, int predictedClass) {
  if (predictedClass < 0 || predictedClass >= numClasses) return 50;
  
  const char* className = classNames[predictedClass];
  
  // 第一步：判断是否为空气
  if (predictedClass == 0 || strcmp(className, "air") == 0) {
    return 50; // 空气 - 中性分数
  }
  
  // 第二步：解析新鲜度状态
  int state = getFreshnessState(className);
  
  // 根据状态计算分数
  switch (state) {
    case 0: // fresh - 高置信度=高分
      return (int)(confidence * 100);
    case 2: // rotten - 高置信度=低分
      return 100 - (int)(confidence * 100);
    case 1: // stale - 中等分数，置信度影响幅度
    default:
      // stale: 基础50分，置信度越高偏离越大
      if (confidence > 0.5f) {
        return 50 - (int)((confidence - 0.5f) * 40); // 置信度高偏向不新鲜
      } else {
        return 50 + (int)((0.5f - confidence) * 40); // 置信度低偏向较新鲜
      }
  }
}
