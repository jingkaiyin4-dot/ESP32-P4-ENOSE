/**
 * espnow-receiver-s3.ino v2.0 — 智能食材新鲜度监测系统 · ESP-NOW 接收端
 *
 * 模块化架构:
 *   config.h        — 全局常量、数据结构、extern 声明
 *   ble_manager     — ESP-NOW 管理器 (Sender↔Receiver↔Bridge)
 *   sd_manager      — SD卡、模型管理、归一化参数、类别加载
 *   model_runner    — TFLite 推理、空气基准、新鲜度评分
 *   web_server      — WiFi 更新模式、HTTP 处理
 *   serial_cmd      — 串口命令调度
 *   relay_control   — UV 继电器控制
 *
 * 硬件: ESP32-S3 (XIAO_ESP32S3) + SD卡 SPI
 * Arduino Core: esp32:esp32:esp32s3
 *
 * 本机 MAC: A8:03:2A:E1:00:02
 */

#include "config.h"
#include "model_updater.h"
#include "atomization.h"
#include "fan_control.h"
#include "ble_manager.h"
#include "sd_manager.h"
#include "model_runner.h"
#include "serial_cmd.h"
// #include "web_server.h"  // WiFi OTA removed - ESP-NOW OTA only
// #include "tdm_test.h"    // Dev test removed
#include "relay_control.h"
#include "servo_control.h"
// #include "display_manager.h"  // TFT removed - causes SPI crash

// ==================== 串口输出限流 ====================
// 高频推理时（~50fps）每帧都 Serial.printf 会严重阻塞主循环
// 限制：推理结果每 SERIAL_LOG_INTERVAL ms 输出一次；$DATA 限流输出
static unsigned long lastSerialLogMs = 0;
#define SERIAL_LOG_INTERVAL 500  // 推理结果日志间隔 500ms
#define DATA_OUTPUT_INTERVAL 500  // $DATA 限流间隔 500ms

// ==================== SD 日志限流 ====================
// SD卡SPI写入慢(20-100ms)，不需要每帧都写，和采集频率匹配即可
#define SD_LOG_INTERVAL 2000  // SD卡写入间隔 2s

// ==================== 串口任务（Core 1） ====================
static void serialTask(void* arg) {
  String lineBuf = "";
  while (true) {
    while (Serial.available() > 0) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (lineBuf.length() > 0) {
          lineBuf.trim();
          handleSerialCommand(lineBuf);
          lineBuf = "";
        }
      } else {
        lineBuf += c;
        // 防止行过长导致内存问题
        if (lineBuf.length() > 200) {
          lineBuf = "";
        }
      }
    }
    delay(1);
  }
}

// ==================== 辅助：输出传感器状态字符串 ====================
String getStatusString(uint8_t status) {
  String s = "";
  if (status & STATUS_ADS1115_OK) s += "ADS1115 ";
  if (status & STATUS_MHZ19C_OK)  s += "MHZ19C ";
  if (status & STATUS_BME680_OK)  s += "BME680 ";
  if (s.length() == 0) s = "None";
  return s;
}

// ==================== setup ====================
void setup() {
  Serial.begin(115200);
  // 超时等待串口就绪，避免无串口监视器时永久阻塞
  unsigned long serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart < 3000)) { delay(10); }
  Serial.println("\n===== ESP32-S3 ESP-NOW Receiver - Food Freshness =====");

  // ---- PSRAM 初始化 ----
  Serial.println("Initializing PSRAM...");
  bool psramOk = false;
#if CONFIG_IDF_TARGET_ESP32S3
  psramOk = psramInit();
  if (!psramOk) {
    Serial.println("psramInit() failed, trying esp_psram_init()...");
    psramOk = (esp_psram_init() == ESP_OK);
  }
#else
  psramOk = psramInit();
#endif
  if (psramOk) {
    Serial.printf("PSRAM: %d bytes total, %d bytes free\n",
                  ESP.getPsramSize(), ESP.getFreePsram());
    if (ESP.getPsramSize() == 0) {
      Serial.println("WARNING: PSRAM init OK but size=0, check Tools > PSRAM = OPI PSRAM");
    }
  } else {
    Serial.println("PSRAM: init FAILED! Check Tools > PSRAM setting.");
  }

  // ---- 初始化默认类别（已改为模型加载时根据输出维度自动生成，此处不再硬编码） ----

  // ---- SD 卡初始化 ----
  if (initSD()) {
    Serial.println("SD OK");

    if (!SD.exists("/test")) SD.mkdir("/test");

    scanModels();
    listModels();

    if (modelCount > 0) {
      if (loadModelByIndex(0)) {
        model_loaded = true;
        modelReady = true;
        pendingModelList = true;  // 启动后立即发送模型列表
      } else {
        Serial.println("Model load failed - will retry in loop");
        model_loaded = false;
        modelReady = false;
      }
    } else {
      Serial.println("No model found on SD - will retry in loop");
      model_loaded = false;
      modelReady = false;
    }

    // 创建数据日志文件
    char filename[64];
    time_t unixTime = time(NULL);
    if (unixTime > 1000000000) {
      struct tm* tm = gmtime(&unixTime);
      snprintf(filename, sizeof(filename),
               "/test/sensor_log_%04d%02d%02d_%02d%02d.csv",
               tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
               tm->tm_hour, tm->tm_min);
    } else {
      String mac = WiFi.macAddress();
      mac.replace(":", "");
      snprintf(filename, sizeof(filename),
               "/test/sensor_%s_%lu.csv", mac.c_str(), (unsigned long)millis());
    }
    data_file = SD.open(filename, FILE_WRITE);
    if (data_file) {
      data_file.println("timestamp,odor,hcho,co,voc,co2,co2_temp,env_temp,humidity,status,pred,freshness");
      data_file.flush();
      Serial.printf("Data log: %s\n", filename);
    }
  } else {
    Serial.println("[WARN] SD card failed - no logging, no model");
  }

  // ---- ESP-NOW 初始化 ----
  initESPNow();

  // ---- UV 继电器初始化 ----
  initRelay();

  // ---- 舵机(开盖换气)初始化 ----
  initServo();
  muInit();  // 模型更新器
  initFog();  // 雾化加湿器
  initFan();  // 12V风扇(3V继电器)

  // ---- 串口命令任务（Core 1） ----
  xTaskCreatePinnedToCore(serialTask, "serialTask", 4096, NULL, 1, NULL, 1);

  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println();
  Serial.println("===== Ready =====");
  espnowSendCmdToSender("set interval 2");  // restart to 2s sampling
  delay(500);
  Serial.println("Waiting for ESP-NOW data...");
}

// ==================== loop ====================
void loop() {
  // ---- 处理 Sender 接收到的数据包 (环形缓冲区) ----
  if (ringCount > 0) {
    SensorData data = sensorRing[ringTail];  // 读取最旧帧
    // 消费一帧
    ringTail = (ringTail + 1) % SENSOR_RING_SIZE;
    ringCount--;

    int   predClass = 0;
    float conf      = 0.0f;
    int   freshness = 0;

    if (model_loaded) {
      predClass = runInference(data);
      conf      = getConfidence();
      freshness = calculateFreshnessScore(conf, predClass);
    }

    // ====== 优先级1: 立即上报P4 (最关键的路径，不能被任何慢操作阻塞) ======
    sendToP4(data, predClass, conf, freshness);

    // ====== 优先级2: 本地控制逻辑 (很快) ======
    if (model_loaded && predClass >= 0 && predClass < numClasses) {
      const char* className = classNames[predClass];
      uvAutoCheck(freshness, className);
      lidAutoCheck(freshness, className);
      fogAutoCheck(data.humidity);       // 湿度低→自动加湿
      fanAutoCheck(freshness, className); // 新鲜度低→自动排风
    }

    // ====== 优先级3: 串口输出 (限流，避免Serial阻塞) ======
    unsigned long nowMs = millis();

    // $DATA 输出限流 — 给GUI采集用，但不需要比采集频率更快
    static unsigned long lastDataOutputMs = 0;
    if (nowMs - lastDataOutputMs >= DATA_OUTPUT_INTERVAL) {
      lastDataOutputMs = nowMs;
      Serial.printf("$DATA:%.2f,%.2f,%.2f,%.2f,%u,%.1f,%.1f,%u |dt=%lu|q=%d\n",
                    data.odor_ppm, data.hcho_ppm, data.co_ppm, data.voc_ppm,
                    data.co2_ppm, data.env_temp, data.humidity, data.timestamp,
                    nowMs - (lastDataOutputMs - DATA_OUTPUT_INTERVAL), (int)ringCount);
    }

    // 推理结果日志限流
    if (model_loaded && nowMs - lastSerialLogMs >= SERIAL_LOG_INTERVAL) {
      lastSerialLogMs = nowMs;
      const char* className = (predClass >= 0 && predClass < numClasses) ? classNames[predClass] : "unknown";
      const char* foodName  = getFoodDisplayName(className);
      int freshState = getFreshnessState(className);
      const char* stateNames[] = { "Fresh", "Stale", "Rotten" };
      if (isAirClass(className)) {
        Serial.printf("-> [Air] fresh=50/100\n");
      } else {
        Serial.printf("-> Type:%s State:%s Conf:%.1f%% Fresh:%d/100\n",
                      foodName, stateNames[freshState], conf * 100, freshness);
      }
    } else if (!model_loaded) {
      // 无模型时也限流
      if (nowMs - lastSerialLogMs >= SERIAL_LOG_INTERVAL) {
        lastSerialLogMs = nowMs;
        Serial.println("-> No model, forwarding raw data");
      }
    }

    // ====== 优先级4: SD卡日志 (最慢的操作，降频到2秒一次) ======
    static unsigned long lastSdLogMs = 0;
    if (nowMs - lastSdLogMs >= SD_LOG_INTERVAL) {
      lastSdLogMs = nowMs;
      logSensorDataToSD(data, predClass, freshness);
    }
  }

  // ---- 处理 Warmup 包 ----
  if (warmupPending) {
    warmupPending = false;
    Serial.printf("\n[Warmup] Remaining: %d sec\n", lastWarmupRemaining);
  }

  // ---- 处理 Warmup 转发到 P4 (从回调解耦) ----
  if (pendingWarmupFwd) {
    pendingWarmupFwd = false;
    sendWarmupToP4(lastWarmupRemaining);
  }

  // ---- 处理 P4 控制命令 (UART接收) ----
  if (p4CmdPending) {
    p4CmdPending = false;
    handleP4UvCommand(String(p4CmdBuffer));
  }

  // ---- UV 继电器定时关闭 ----
  uvLoop();

  // ---- 舵机自动关盖定时 ----
  lidLoop();

  // ---- 雾化加湿定时 ----
  fogLoop();

  // ---- 风扇定时关闭 ----
  fanLoop();

  // 模型更新超时检测
  muCheckTimeout();

  // ---- 模型列表发送 (仅在模型就绪/切换/OTA后触发一次，逐帧发送) ----
  if (pendingModelList && mlSendState == MLS_IDLE) {
    pendingModelList = false;
    triggerModelListSend();
  }
  modelListSendLoop();

  // ---- 模型加载重试 (模型未就绪时) ----
  static unsigned long lastModelRetryMs = 0;
  static int modelRetryCount = 0;
  if (!model_loaded && !modelReady && sd_ready && (millis() - lastModelRetryMs >= MODEL_RETRY_INTERVAL)) {
    lastModelRetryMs = millis();
    modelRetryCount++;
    // 仅首次和每10次重试输出详细日志，其余只输出简短提示
    if (modelRetryCount <= 1 || modelRetryCount % 10 == 0) {
      Serial.printf("[ModelRetry] #%d Retrying SD init + model load...\n", modelRetryCount);
    }
    
    // 重新初始化SD卡
    if (initSD()) {
      scanModels();
      if (modelCount > 0) {
        if (loadModelByIndex(0)) {
          model_loaded = true;
          modelReady = true;
          Serial.printf("[ModelRetry] #%d OK: Loaded [%d] %s\n", modelRetryCount, currentModelIndex, currentModelFile);
          // 模型就绪后立即上报列表
          pendingModelList = true;
          modelRetryCount = 0;
        }
      }
    }
  }
  
  // ---- SD卡未初始化时也重试 ----
  static unsigned long lastSdRetryMs = 0;
  static int sdRetryCount = 0;
  if (!sd_ready && (millis() - lastSdRetryMs >= MODEL_RETRY_INTERVAL)) {
    lastSdRetryMs = millis();
    sdRetryCount++;
    if (sdRetryCount <= 1 || sdRetryCount % 10 == 0) {
      Serial.printf("[ModelRetry] #%d SD not ready, retrying...\n", sdRetryCount);
    }
    if (initSD()) {
      scanModels();
      if (modelCount > 0 && loadModelByIndex(0)) {
        model_loaded = true;
        modelReady = true;
        pendingModelList = true;
        sdRetryCount = 0;
        Serial.printf("[ModelRetry] SD #%d OK: model loaded\n", sdRetryCount);
      }
    }
  }

  csvRotateCheck();
  delay(2);
}
