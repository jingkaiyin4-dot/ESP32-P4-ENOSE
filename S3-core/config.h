/**
 * config.h âå±äº«å¸¸éãæ°æ®ç»æä¸ extern å£°æ
 * æ¨¡åå?BLE æ¥æ¶ç«?- æºè½é£ææ°é²åº¦çæµç³»ç»? */
#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>

// ==================== æ°æ®åç±»åæ å¿?====================
#define STATUS_ADS1115_OK 0x01
#define STATUS_MHZ19C_OK  0x02
#define STATUS_BME680_OK  0x04
#define PKT_TYPE_SENSOR   0x01
#define PKT_TYPE_WARMUP   0x02
#define PKT_TYPE_COMMAND  0x03

// ==================== æ°æ®ç»æ ====================
typedef struct __attribute__((packed)) {
  uint8_t  dataType;
  float    odor_ppm;
  float    hcho_ppm;
  float    co_ppm;
  float    voc_ppm;
  uint16_t co2_ppm;
  int16_t  co2_temp;
  float    env_temp;
  float    humidity;
  uint8_t  sensor_status;
  uint32_t timestamp;
} SensorData;

typedef struct __attribute__((packed)) {
  uint8_t  dataType;
  uint16_t remainingSec;
  uint32_t timestamp;
} WarmupStatus;

typedef struct __attribute__((packed)) {
  uint8_t  dataType;
  char     command[32];
  uint32_t timestamp;
} CommandPacket;

// ==================== ESP-NOW éç½® ====================
// èªå®ä¹?MAC å°å
// DEVICE_NAME (QR pairing)
#define DEVICE_NAME  "S3_03"

#define RECEIVER_MAC_0  0xA8
#define RECEIVER_MAC_1  0x03
#define RECEIVER_MAC_2  0x2A
#define RECEIVER_MAC_3  0xE1
#define RECEIVER_MAC_4  0x00
#define RECEIVER_MAC_5  0x02
#define SENDER_MAC_0    0xA8
#define SENDER_MAC_1    0x03
#define SENDER_MAC_2    0x2A
#define SENDER_MAC_3    0xE1
#define SENDER_MAC_4    0x00
#define SENDER_MAC_5    0x01
#define BRIDGE_MAC_0    0xA8
#define BRIDGE_MAC_1    0x03
#define BRIDGE_MAC_2    0x2A
#define BRIDGE_MAC_3    0xE1
#define BRIDGE_MAC_4    0x00
#define BRIDGE_MAC_5    0x03

// ==================== SD/模型配置 ====================
// XIAO ESP32-S3 板载MicroSD座 (SPI模式)
//   D3(CS)=GPIO10, D0(MISO)=GPIO11, CLK(SCK)=GPIO12, CMD(MOSI)=GPIO13
#define SD_SCK   12
#define SD_MISO  11
#define SD_MOSI  13
#define SD_CS    10
#define MODEL_FILE       "/model.tflite"
#define MODEL_DIR        "/"
#define MAX_MODELS       6
#define TENSOR_ARENA_SIZE 100000
#define MAX_CLASSES      20

// ==================== UV ç»§çµå¨éç½?====================
#define RELAY_PIN        1       // GPIO1 (D1 on XIAO ESP32-S3)
#define RELAY_ACTIVE_LOW false   // 三极管高电平触发: HIGH=导通灯亮, LOW=截止灯灭
#define UV_ON_DURATION   30000   // UVç¯åæ¬¡æ¶æ¯æ¶é?30ç§?ms)
#define FRESH_THRESHOLD  60      // æ°é²åº¦ä½äºæ­¤å¼è§¦åæ¶æ¯?
// ==================== èµæº(å¼çæ¢æ°?éç½® ====================
#define SERVO_PIN        4       // GPIO4 (D2 on XIAO ESP32-S3)
#define MODEL_UPDATE_MAX_SIZE  (512 * 1024)  // 最大 512KB (预留 473KB 模型能力)
#define MODEL_CHUNK_SIZE      100          // 每个 ESP-NOW chunk 字节 (Hex编码后200字节+报头≤250B ESP-NOW上限)
#define MODEL_UPDATE_TIMEOUT  60000        // 模型更新超时 60s (473KB模型需要更长传输时间)
#define MODEL_TMP_SUFFIX      ".tmp"        // 临时下载文件后缀
#define MODEL_BAK_SUFFIX      ".bak"        // 旧模型备份后缀

// ==================== 模型就绪与重试配置 ====================
#define MODEL_RETRY_INTERVAL   5000        // 模型加载重试间隔 5s

#define SERVO_CLOSE_PULSE 500    // å³é­(0Â°)èå®½ us
#define SERVO_OPEN_PULSE  2450   // æå¼(æå¤§è§åº?èå®½ us
#define LID_AUTO_OPEN_THRESHOLD 40  // æ°é²åº¦â¤æ­¤å¼èªå¨å¼çæ¢æ°
#define LID_AUTO_CLOSE_DELAY  60000 // èªå¨å¼çå60ç§å³ç?ms)

// ==================== æ¨¡åä¿¡æ¯ç»æä½?====================
struct ModelInfo {
  char   filename[32];
  char   label[32];
  size_t filesize;
};

// ==================== å½ä¸ååæ°ç»æä½ ====================
struct NormParams {
  float min[16];
  float max[16];
  bool  loaded;
};

// ==================== ESP-NOW æ¨¡åå¨å±åé (extern) ====================
#include <esp_now.h>
#include <esp_mac.h>  // esp_base_mac_addr_set (ESP-IDF v5.x)

extern bool espnowReady;

// æ¨çç¼å²å?// 推理环形缓冲区 (定义在 ble_manager.h)
extern SensorData       sensorRing[];
extern volatile int     ringHead;
extern volatile int     ringTail;
extern volatile int     ringCount;
// 兼容旧代码别名
#define pendingInference  (ringCount > 0)
#define pendingData       sensorRing[ringTail]
extern volatile bool    warmupPending;
extern volatile bool    pendingWarmupFwd;  // warmup待转发到P4标志
extern uint16_t         lastWarmupRemaining;

// P4æ§å¶å½ä»¤ç¼å²
extern volatile bool    p4CmdPending;
extern char             p4CmdBuffer[256];

// ç»è®¡
extern volatile uint32_t rxPktCount;
extern volatile uint32_t rxDataCount;
extern volatile uint32_t rxWarmupCount;

// ææ°æ°æ?extern SensorData latestData;
extern bool       hasValidSensorData;

// ==================== SD/æ¨¡åæ¨¡åå¨å±åé (extern) ====================
extern SPIClass*  sd_spi;
extern bool       sd_ready;
extern File       data_file;

extern ModelInfo  modelList[MAX_MODELS];
extern int        modelCount;
extern int        currentModelIndex;
extern char       currentModelFile[32];

extern NormParams normParams;

extern const char* classNames[MAX_CLASSES];
extern int         numClasses;
extern char        classNamesBuffer[MAX_CLASSES][32];

// ==================== æ¨¡åæ¨çæ¨¡åå¨å±åé (extern) ====================
#include <TensorFlowLite_ESP32.h>
#include <tensorflow/lite/experimental/micro/kernels/all_ops_resolver.h>
#include <tensorflow/lite/experimental/micro/micro_error_reporter.h>
#include <tensorflow/lite/experimental/micro/micro_interpreter.h>
#include <tensorflow/lite/schema/schema_generated.h>
#include <tensorflow/lite/version.h>

using namespace tflite;

extern const Model*      tflite_model;
extern MicroInterpreter* interpreter;
extern uint8_t tensor_arena[TENSOR_ARENA_SIZE];
extern uint8_t*          model_buffer;
extern bool              model_loaded;
extern float             last_confidence;
extern int               last_predicted;

// ==================== éç¨è¾å© ====================
String getStatusString(uint8_t status);

// ==================== UV ç»§çµå¨æ¨¡åå¨å±åé (extern) ====================
extern bool uvRelayOn;
extern bool uvAutoMode;
extern unsigned long uvStartTime;
extern unsigned long uvOnDuration;
extern int uvFreshThreshold;
extern bool p4Online;

// ==================== èµæºæ¨¡åå¨å±åé (extern) ====================
extern bool lidOpen;           // å½åçå­ç¶æ?extern bool lidAutoMode;       // èªå¨æ¢æ°æ¨¡å¼
extern unsigned long lidOpenTime;

struct ModelUpdateContext;  // from model_updater.h
extern ModelUpdateContext muCtx;
extern bool pendingModelInfo;
extern bool pendingModelList;

// ==================== 模型就绪状态 ====================
extern bool modelReady;              // 模型是否就绪（加载成功）

// ==================== 模型列表分片发送状态 ====================
enum ModelListSendState {
  MLS_IDLE = 0,
  MLS_SUMMARY,
  MLS_DETAIL,
  MLS_END
};
extern volatile ModelListSendState mlSendState;
extern int mlSendIndex;

// ==================== PSRAM æ¯æ ====================
#include <esp_heap_caps.h>
#include <esp_psram.h>

#ifndef BOARD_HAS_PSRAM
#define BOARD_HAS_PSRAM 1
#endif

#endif // CONFIG_H
