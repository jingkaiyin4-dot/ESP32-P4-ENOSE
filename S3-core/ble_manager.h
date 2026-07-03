/**
 * ble_manager.h → ESP-NOW 管理器
 * Receiver 通过 ESP-NOW 接收 Sender 数据、发送 JSON 到 Bridge、收发命令
 */
#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include "config.h"
#include "relay_control.h"

// ==================== MAC 地址数组 ====================
uint8_t RECEIVER_MAC[] = {RECEIVER_MAC_0, RECEIVER_MAC_1, RECEIVER_MAC_2,
                           RECEIVER_MAC_3, RECEIVER_MAC_4, RECEIVER_MAC_5};
uint8_t SENDER_MAC[]   = {SENDER_MAC_0, SENDER_MAC_1, SENDER_MAC_2,
                           SENDER_MAC_3, SENDER_MAC_4, SENDER_MAC_5};
uint8_t BRIDGE_MAC[]   = {BRIDGE_MAC_0, BRIDGE_MAC_1, BRIDGE_MAC_2,
                           BRIDGE_MAC_3, BRIDGE_MAC_4, BRIDGE_MAC_5};

bool espnowReady = false;

// ==================== 前向声明 ====================
void sendWarmupToP4(uint16_t remainingSec);

// ==================== P4控制命令缓冲 ====================
volatile bool     p4CmdPending = false;
char             p4CmdBuffer[256] = {0};

// ==================== 推理环形缓冲区 ====================
// 旧方案: pendingInference 单标志，处理慢时新数据直接丢弃
// 新方案: 环形缓冲区，容量4帧，确保2秒采样不丢帧
#define SENSOR_RING_SIZE 4
SensorData sensorRing[SENSOR_RING_SIZE];
volatile int  ringHead = 0;  // 回调写入位 (ISR侧)
volatile int  ringTail = 0;  // loop读取位
volatile int  ringCount = 0; // 缓冲帧数

volatile bool    warmupPending      = false;
volatile bool    pendingWarmupFwd   = false;  // warmup待转发到P4标志
bool pendingModelInfo = false;
bool pendingModelList = false;
uint16_t         lastWarmupRemaining = 0;

// ==================== 模型就绪状态 ====================
bool          modelReady       = false;    // 模型是否就绪

// ==================== 模型列表分片发送状态变量 ====================
// ModelListSendState 枚举定义在 config.h
volatile ModelListSendState mlSendState = MLS_IDLE;
int mlSendIndex = 0;  // 当前发送到第几个 model_detail

// ==================== 统计计数 ====================
volatile uint32_t rxPktCount   = 0;
volatile uint32_t rxDataCount  = 0;
volatile uint32_t rxWarmupCount = 0;

// ==================== 最新数据 ====================
SensorData latestData;
bool       hasValidSensorData = false;

struct P4Sample {
  float odor, hcho, co, voc;
  uint16_t co2;
  int   predClass;
  float conf;
  int   freshness;
};

// ==================== ESP-NOW 发送回调 ====================
void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
  // 静默，避免刷屏
}

// ==================== ESP-NOW 接收回调 ====================
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < 1) return;

  // 判断来源：Sender 还是 Bridge
  bool fromSender = (memcmp(info->src_addr, SENDER_MAC, 6) == 0);
  bool fromBridge = (memcmp(info->src_addr, BRIDGE_MAC, 6) == 0);

  if (fromSender) {
    // --- Sender 发来的 SensorData / WarmupStatus / Command ---
    uint8_t dataType = data[0];
    rxPktCount++;
    if (dataType == PKT_TYPE_SENSOR && len == (int)sizeof(SensorData)) {
      // 环形缓冲区写入：满则覆盖最旧帧（ringTail前移）
      if (ringCount < SENSOR_RING_SIZE) {
        memcpy(&sensorRing[ringHead], data, sizeof(SensorData));
        ringHead = (ringHead + 1) % SENSOR_RING_SIZE;
        ringCount++;
      } else {
        // 缓冲区满：覆盖最旧帧（处理跟不上采样频率时）
        memcpy(&sensorRing[ringHead], data, sizeof(SensorData));
        ringHead = (ringHead + 1) % SENSOR_RING_SIZE;
        ringTail = (ringTail + 1) % SENSOR_RING_SIZE;
        // ringCount 不变，保持 SENSOR_RING_SIZE
      }
      latestData = sensorRing[(ringHead - 1 + SENSOR_RING_SIZE) % SENSOR_RING_SIZE];
      hasValidSensorData = true;
      rxDataCount++;
    } else if (dataType == PKT_TYPE_WARMUP && len == (int)sizeof(WarmupStatus)) {
      WarmupStatus ws;
      memcpy(&ws, data, sizeof(ws));
      lastWarmupRemaining = ws.remainingSec;
      warmupPending = true;
      rxWarmupCount++;
      // 不在回调中转发warmup到P4，改为在loop中处理
      pendingWarmupFwd = true;
    } else if (dataType == PKT_TYPE_COMMAND && len == (int)sizeof(CommandPacket)) {
      // S3(Sender)转发的服务器UV指令 — 缓冲命令，不在回调中处理
      CommandPacket cpkt;
      memcpy(&cpkt, data, sizeof(cpkt));
      // 仅拷贝到 p4CmdBuffer，不在回调中创建 String
      int cmdLen = strnlen(cpkt.command, sizeof(cpkt.command));
      if (cmdLen > 0) {
        strncpy(p4CmdBuffer, cpkt.command, sizeof(p4CmdBuffer) - 1);
        p4CmdBuffer[sizeof(p4CmdBuffer) - 1] = '\0';
        p4CmdPending = true;
      }
    }
  } else if (fromBridge) {
    // --- Bridge 发来的 P4 命令 ---
    if (len > 0 && len < (int)sizeof(p4CmdBuffer)) {
      memcpy(p4CmdBuffer, data, len);
      p4CmdBuffer[len] = '\0';
      p4CmdPending = true;
    }
  }
}

// ==================== 初始化 ESP-NOW ====================
void initESPNow() {
  esp_base_mac_addr_set(RECEIVER_MAC);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] Init FAILED");
    return;
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  // 添加 Sender 为对端
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, SENDER_MAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) == ESP_OK) {
    Serial.println("[ESPNOW] Peer Sender added");
  }

  // 添加 Bridge 为对端
  memcpy(peer.peer_addr, BRIDGE_MAC, 6);
  if (esp_now_add_peer(&peer) == ESP_OK) {
    Serial.println("[ESPNOW] Peer Bridge added");
  }

  espnowReady = true;
  Serial.println("[ESPNOW] Init OK");
  Serial.printf("  My MAC:    %02X:%02X:%02X:%02X:%02X:%02X\n",
    RECEIVER_MAC[0],RECEIVER_MAC[1],RECEIVER_MAC[2],
    RECEIVER_MAC[3],RECEIVER_MAC[4],RECEIVER_MAC[5]);
  Serial.printf("  Sender:    %02X:%02X:%02X:%02X:%02X:%02X\n",
    SENDER_MAC[0],SENDER_MAC[1],SENDER_MAC[2],
    SENDER_MAC[3],SENDER_MAC[4],SENDER_MAC[5]);
  Serial.printf("  Bridge:    %02X:%02X:%02X:%02X:%02X:%02X\n",
    BRIDGE_MAC[0],BRIDGE_MAC[1],BRIDGE_MAC[2],
    BRIDGE_MAC[3],BRIDGE_MAC[4],BRIDGE_MAC[5]);
}

// ==================== 发送命令到 Sender ====================
void espnowSendCmdToSender(const String& cmd) {
  if (!espnowReady) {
    Serial.println("[ESPNOW] Not ready, cannot send cmd to Sender");
    return;
  }
  esp_now_send(SENDER_MAC, (uint8_t*)cmd.c_str(), cmd.length());
  Serial.printf("[ESPNOW→Sender] %s\n", cmd.c_str());
}

// ==================== 发送 Warmup 状态到 P4 (ESP-NOW→Bridge) ====================
void sendWarmupToP4(uint16_t remainingSec) {
  char jsonBuf[80];
  snprintf(jsonBuf, sizeof(jsonBuf),
    "{\"name\":\"%s\",\"type\":\"warmup\",\"remaining\":%u}", DEVICE_NAME, remainingSec);
  if (espnowReady) {
    esp_now_send(BRIDGE_MAC, (uint8_t*)jsonBuf, strlen(jsonBuf));
    Serial.printf("[ESPNOW→Bridge] %s\n", jsonBuf);
  }
}

// ==================== 触发模型列表发送 ====================
// 仅设置标志，实际发送在 loop() 的 modelListSendLoop() 中逐帧完成
void triggerModelListSend() {
  if (mlSendState != MLS_IDLE) {
    // 正在发送中，不重复触发
    return;
  }
  mlSendState = MLS_SUMMARY;
  mlSendIndex = 0;
}

// ==================== 模型列表分片发送 (每loop一帧) ====================
// 在 loop() 中调用，每次只发一个 ESP-NOW 包，不阻塞
void modelListSendLoop() {
  if (!espnowReady || mlSendState == MLS_IDLE) return;
  
  switch (mlSendState) {
    case MLS_SUMMARY: {
      char buf[128];
      snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"type\":\"model_list\",\"total\":%d,\"active\":%d}",
        DEVICE_NAME, modelCount, currentModelIndex);
      esp_now_send(BRIDGE_MAC, (uint8_t*)buf, strlen(buf));
      Serial.printf("[ESPNOW→Bridge] %s\n", buf);
      mlSendState = MLS_DETAIL;
      mlSendIndex = 0;
      break;
    }
    case MLS_DETAIL: {
      if (mlSendIndex >= modelCount) {
        mlSendState = MLS_END;
        break;
      }
      char mName[32];
      strncpy(mName, modelList[mlSendIndex].label, 31);
      mName[31] = '\0';
      
      char buf[200];
      if (mlSendIndex == currentModelIndex) {
        // 安全拼接类别名，防止缓冲区溢出
        char classesStr[80] = "";
        size_t classesLen = 0;
        for (int j = 0; j < numClasses && j < 10; j++) {
          size_t nameLen = strlen(classNames[j]);
          if (classesLen + nameLen + 2 >= sizeof(classesStr)) break;
          if (j > 0) { strcat(classesStr, ","); classesLen++; }
          strcat(classesStr, classNames[j]); classesLen += nameLen;
        }
        snprintf(buf, sizeof(buf),
          "{\"name\":\"%s\",\"type\":\"model_detail\",\"idx\":%d,\"model\":\"%s\",\"size\":%zu,"
          "\"classes\":\"%s\",\"active\":1}",
          DEVICE_NAME, mlSendIndex, mName, modelList[mlSendIndex].filesize, classesStr);
      } else {
        snprintf(buf, sizeof(buf),
          "{\"name\":\"%s\",\"type\":\"model_detail\",\"idx\":%d,\"model\":\"%s\",\"size\":%zu,\"active\":0}",
          DEVICE_NAME, mlSendIndex, mName, modelList[mlSendIndex].filesize);
      }
      esp_now_send(BRIDGE_MAC, (uint8_t*)buf, strlen(buf));
      Serial.printf("[ESPNOW→Bridge] %s\n", buf);
      mlSendIndex++;
      break;
    }
    case MLS_END: {
      char buf[64];
      snprintf(buf, sizeof(buf), "{\"name\":\"%s\",\"type\":\"model_list_end\"}", DEVICE_NAME);
      esp_now_send(BRIDGE_MAC, (uint8_t*)buf, strlen(buf));
      Serial.printf("[ESPNOW→Bridge] %s\n", buf);
      
      // model_list_end snprintf now needs DEVICE_NAME param
      mlSendState = MLS_IDLE;
      Serial.println("[ModelList] Sent to P4");
      break;
    }
    default:
      mlSendState = MLS_IDLE;
      break;
  }
}

// ==================== 发送传感器数据到 P4 ====================
// 关键路径！必须在推理后第一时间调用，不能被SD/Serial等慢操作阻塞
void sendToP4(const SensorData& data, int predClass, float confidence, int freshness) {
  // 当前分类名
  const char* mainClassName = "unknown";
  if (model_loaded && predClass >= 0 && predClass < numClasses) {
    mainClassName = classNames[predClass];
  } else if (!model_loaded) {
    mainClassName = "air";
  }

  // UV 剩余时间
  unsigned long uvRemain = 0;
  if (uvRelayOn) {
    unsigned long elapsed = millis() - uvStartTime;
    uvRemain = (elapsed < uvOnDuration) ? (uvOnDuration - elapsed) / 1000 : 0;
  }

  // 模型更新ACK已改为独立发送(muSendAck), 不再嵌入传感器JSON

  // ---- 模型信息字段 (单模型上报，model info / model switch 后触发) ----
  char miName[32] = "";
  char miClasses[128] = "";
  int  miSize = 0;
  bool sendMi = pendingModelInfo;
  if (pendingModelInfo) {
    pendingModelInfo = false;
    if (currentModelFile[0]) {
      const char* slash = strrchr(currentModelFile, '/');
      const char* baseName = slash ? slash + 1 : currentModelFile;
      strncpy(miName, baseName, 31);
      miName[31] = '\0';
      char* dot = strstr(miName, ".tflite");
      if (dot) *dot = '\0';
    }
    if (modelCount > 0 && currentModelIndex >= 0 && currentModelIndex < modelCount) {
      miSize = (int)modelList[currentModelIndex].filesize;
    }
    for (int i = 0; i < numClasses; i++) {
      if (i > 0) strcat(miClasses, ",");
      strcat(miClasses, classNames[i]);
    }
  }

  char jsonBuf[640];
  snprintf(jsonBuf, sizeof(jsonBuf),
    "{\"name\":\"%s\",\"o\":%.2f,\"h\":%.2f,\"c\":%.2f,\"v\":%.2f,\"co2\":%u,"
    "\"t\":%.2f,\"hu\":%.2f,"
    "\"cls\":\"%s\",\"conf\":%.2f,\"fr\":%d,"
    "\"uv\":%d,\"ua\":%d,\"ur\":%lu,\"ud\":%lu,"
    "\"lo\":%d,"
    "\"pm\":%d,"
    "\"mr\":%d,"
    "\"fo\":%d,\"fa\":%d,"
    "\"fn\":%d,\"fl\":%d,"
    "\"mi\":%d,\"mn\":\"%s\",\"ms\":%d,\"mc\":\"%s\"}",
    DEVICE_NAME, data.odor_ppm, data.hcho_ppm, data.co_ppm, data.voc_ppm, data.co2_ppm,
    data.env_temp, data.humidity,
    mainClassName, confidence, freshness,
    uvRelayOn ? 1 : 0,
    uvAutoMode ? 1 : 0,
    uvRemain,
    uvOnDuration / 1000,
    lidOpen ? 1 : 0,   // lo=lid open
    p4Online ? 1 : 0,  // pm=P4 mode
    modelReady ? 1 : 0, // mr=model ready
    fogOn ? 1 : 0,     // fo=fog on
    fogAutoMode ? 1 : 0, // fa=fog auto
    fanOn ? 1 : 0,     // fn=fan on
    fanAutoMode ? 1 : 0, // fl=fan auto
    sendMi ? 1 : 0, miName, miSize, miClasses);

  if (espnowReady) {
    esp_now_send(BRIDGE_MAC, (uint8_t*)jsonBuf, strlen(jsonBuf));
    // 限流：ESP-NOW发送日志每 2s 输出一次，避免串口阻塞
    {
      static unsigned long lastBridgeLogMs = 0;
      if (millis() - lastBridgeLogMs >= 2000) {
        lastBridgeLogMs = millis();
        Serial.printf("[→Bridge] cls=%s fr=%d mr=%d\n",
                      mainClassName, freshness, modelReady ? 1 : 0);
      }
    }
  }
}

#endif
