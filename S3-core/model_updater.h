/**
 * model_updater.h — 流式分片接收模型 + SD原子写入 + TFLite热重载
 * v2.0 — 对齐 P4→C6→S3 OTA 技术规格书
 * 
 * 通信链路: P4(云端/串口) → C6 Bridge(UART) → ESP-NOW → Receiver
 * 
 * 协议:
 *   阶段1 握手: "model update <name> <total_chunks> <total_size> <version>"
 *              → S3回复 {"mt":"chunk_ok","mid":0}
 *   阶段2 分片: "model chunk <chunk_id> <hex_data>"
 *              → S3回复 {"mt":"chunk_ok","mid":<chunk_id>}
 *   阶段3 校验+热部署:
 *              → 成功: {"mt":"model_done"}
 *              → 失败: {"mt":"model_fail"}
 *   阶段4 类名: "model classes <name> air,banana_fresh,banana_rotten,banana_stale"
 *              → S3保存到 /{name}_norm.json, 覆盖运行时类名
 * 
 * 设计决策:
 *   - PSRAM全量缓冲 + 原子写入(.tmp→rename)，不逐片写SD卡
 *   - ACK独立发送(不嵌入传感器JSON)，实时性更好
 *   - 支持473KB模型(max 512KB)，状态机5态
 *   - 模型保存为 /{name}.tflite，支持多模型管理
 *   - 接收完成后先发最后一个chunk ACK，再校验部署
 */
#ifndef MODEL_UPDATER_H
#define MODEL_UPDATER_H

#include "config.h"

// ==================== 模型更新状态机 ====================
enum ModelUpdateState {
  MUS_IDLE = 0,       // 空闲，可接受新更新
  MUS_HANDSHAKE,      // 握手元数据已解析，等待分片
  MUS_RECEIVING,      // 接收分片中
  MUS_VERIFYING,      // 校验文件完整性 + 写入SD
  MUS_DEPLOYING       // 热重载TFLite模型
};

// ==================== 模型更新上下文 ====================
struct ModelUpdateContext {
  ModelUpdateState state;
  
  // 握手元数据
  char    modelName[32];        // 模型名称 (如 "banana_fresh")
  char    modelVersion[16];     // 模型版本 (如 "1.0.0")
  int     totalChunks;          // 分片总数
  size_t  totalSize;            // 模型二进制总大小(字节)
  
  // 接收进度
  int     nextChunkExpected;    // 下一个期望的 chunk ID
  size_t  bytesReceived;        // 已接收字节数
  int     chunksReceived;       // 已接收分片数
  
  // PSRAM 缓冲区
  uint8_t* buffer;              // 全量缓冲区 (PSRAM优先)
  size_t  bufferSize;           // 缓冲区大小
  
  // 超时与错误
  unsigned long lastActivityMs; // 最后活动时间(超时检测)
  int     consecutiveErrors;    // 连续错误计数
  bool    finalized;            // 是否已完成最终处理(防重入)
};

// muCtx 定义 (config.h中有extern前向声明)
ModelUpdateContext muCtx;

// ==================== 前向声明 ====================
static void muSendAck(const char* type, int chunkId);
static void muFinalize();
static void muCleanup();
static uint8_t hexCharToVal(char c);
static size_t hexDecode(const char* hex, size_t hexLen, uint8_t* out, size_t maxOut);

// ==================== 初始化 ====================
void muInit() {
  memset(&muCtx, 0, sizeof(muCtx));
  muCtx.state = MUS_IDLE;
  muCtx.buffer = nullptr;
}

// ==================== Hex字符→数值 ====================
static uint8_t hexCharToVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

// ==================== Hex字符串→二进制 ====================
// 返回解码后的字节数，-1表示出错
static size_t hexDecode(const char* hex, size_t hexLen, uint8_t* out, size_t maxOut) {
  if (hexLen % 2 != 0) return 0;  // Hex长度必须是偶数
  size_t binLen = hexLen / 2;
  if (binLen > maxOut) return 0;
  
  for (size_t i = 0; i < binLen; i++) {
    out[i] = (hexCharToVal(hex[i * 2]) << 4) | hexCharToVal(hex[i * 2 + 1]);
  }
  return binLen;
}

// ==================== 独立发送ACK (通过ESP-NOW直接发JSON) ====================
static void muSendAck(const char* type, int chunkId) {
  if (!espnowReady) {
    Serial.printf("[MU] WARN: ESP-NOW not ready, ACK not sent: %s mid=%d\n", DEVICE_NAME, type, chunkId);
    return;
  }
  
  char buf[64];
  if (chunkId >= 0) {
    snprintf(buf, sizeof(buf), "{\"name\":\"%s\",\"mt\":\"%s\",\"mid\":%d}", DEVICE_NAME, type, chunkId);
  } else {
    snprintf(buf, sizeof(buf), "{\"name\":\"%s\",\"mt\":\"%s\"}", DEVICE_NAME, type);
  }
  
  // 使用config.h中定义的BRIDGE_MAC数组
  uint8_t bridgeAddr[] = {BRIDGE_MAC_0, BRIDGE_MAC_1, BRIDGE_MAC_2,
                          BRIDGE_MAC_3, BRIDGE_MAC_4, BRIDGE_MAC_5};
  esp_now_send(bridgeAddr, (uint8_t*)buf, strlen(buf));
  Serial.printf("[MU→P4] %s\n", buf);
}

// ==================== 处理模型更新命令 ====================
void muHandleCommand(const String& cmd) {
  muCtx.lastActivityMs = millis();
  
  // ============================================================
  // 阶段1: 握手元数据 "model update <name> <chunks> <size> <ver>"
  // ============================================================
  if (cmd.startsWith("model update ")) {
    // 如果正在接收中，先清理
    if (muCtx.state == MUS_RECEIVING || muCtx.state == MUS_HANDSHAKE) {
      Serial.println("[MU] WARN: Update in progress, aborting previous");
      muCleanup();
    }
    
    // 格式: "model update banana_fresh 2368 473600 1.0.0"
    int p1 = cmd.indexOf(' ', 13);  // 跳过 "model update "
    if (p1 < 0) { muSendAck("model_fail", -1); return; }
    String name = cmd.substring(13, p1);
    
    int p2 = cmd.indexOf(' ', p1 + 1);
    if (p2 < 0) { muSendAck("model_fail", -1); return; }
    int totalChunks = cmd.substring(p1 + 1, p2).toInt();
    
    int p3 = cmd.indexOf(' ', p2 + 1);
    size_t totalSize;
    String version;
    if (p3 > 0) {
      totalSize = (size_t)cmd.substring(p2 + 1, p3).toInt();
      version = cmd.substring(p3 + 1);
    } else {
      totalSize = (size_t)cmd.substring(p2 + 1).toInt();
      version = "0.0.0";
    }
    version.trim();
    
    // ---- 参数校验 ----
    if (totalChunks <= 0 || totalChunks > 4096) {
      Serial.printf("[MU] ERR: invalid chunks=%d\n", totalChunks);
      muSendAck("model_fail", -1);
      return;
    }
    if (totalSize <= 0 || totalSize > MODEL_UPDATE_MAX_SIZE) {
      Serial.printf("[MU] ERR: invalid size=%zu (max=%d)\n", totalSize, MODEL_UPDATE_MAX_SIZE);
      muSendAck("model_fail", -1);
      return;
    }
    
    // ---- 分配PSRAM缓冲区 ----
    if (muCtx.buffer) {
      free(muCtx.buffer);
      muCtx.buffer = nullptr;
    }
    
    // 优先PSRAM，不够再试内部RAM
    muCtx.buffer = (uint8_t*)heap_caps_malloc(totalSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!muCtx.buffer) {
      Serial.println("[MU] PSRAM alloc failed, trying internal RAM...");
      muCtx.buffer = (uint8_t*)malloc(totalSize);
    }
    if (!muCtx.buffer) {
      Serial.printf("[MU] ERR: malloc(%zu) failed\n", totalSize);
      muSendAck("model_fail", -1);
      return;
    }
    muCtx.bufferSize = totalSize;
    
    // ---- 初始化上下文 ----
    strncpy(muCtx.modelName, name.c_str(), 31);
    muCtx.modelName[31] = '\0';
    strncpy(muCtx.modelVersion, version.c_str(), 15);
    muCtx.modelVersion[15] = '\0';
    muCtx.totalChunks = totalChunks;
    muCtx.totalSize = totalSize;
    muCtx.nextChunkExpected = 0;
    muCtx.bytesReceived = 0;
    muCtx.chunksReceived = 0;
    muCtx.consecutiveErrors = 0;
    muCtx.finalized = false;
    muCtx.state = MUS_HANDSHAKE;
    
    Serial.printf("[MU] Handshake OK: %s v%s, %d chunks, %zu bytes, PSRAM=%s\n",
                  muCtx.modelName, muCtx.modelVersion, totalChunks, totalSize,
                  heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false) ? "OK" : "?");
    Serial.printf("[MU] PSRAM free: %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // 握手就绪，回复 chunk_ok mid=0，P4开始发送第0号分片
    muCtx.state = MUS_RECEIVING;
    muSendAck("chunk_ok", 0);
    return;
  }
  
  // ============================================================
  // 阶段2: 数据分片 "model chunk <chunk_id> <hex_data>"
  // ============================================================
  if (cmd.startsWith("model chunk ") && muCtx.state == MUS_RECEIVING) {
    int p1 = cmd.indexOf(' ', 12);  // 跳过 "model chunk "
    if (p1 < 0) {
      muSendAck("model_fail", -1);
      return;
    }
    
    int chunkId = cmd.substring(12, p1).toInt();
    const char* hexStr = cmd.c_str() + p1 + 1;
    size_t hexLen = cmd.length() - p1 - 1;
    
    // ---- 按序到达: 正常处理 ----
    if (chunkId == muCtx.nextChunkExpected) {
      // Hex长度校验
      size_t maxBinLen = MODEL_CHUNK_SIZE;
      if (hexLen > maxBinLen * 2) {
        Serial.printf("[MU] ERR: chunk %d hex too long (%zu > %zu)\n",
                      chunkId, hexLen, maxBinLen * 2);
        muSendAck("chunk_retry", chunkId);
        muCtx.consecutiveErrors++;
        return;
      }
      
      // 计算本分片应写入的偏移量
      size_t writeOffset = (size_t)chunkId * MODEL_CHUNK_SIZE;
      
      // 计算本分片最大可用空间
      size_t remaining = muCtx.totalSize - writeOffset;
      size_t thisMax = (remaining < MODEL_CHUNK_SIZE) ? remaining : MODEL_CHUNK_SIZE;
      
      // Hex解码直接写入缓冲区对应位置
      size_t binLen = hexDecode(hexStr, hexLen,
                                muCtx.buffer + writeOffset, thisMax);
      if (binLen == 0 && hexLen > 0) {
        Serial.printf("[MU] ERR: chunk %d hex decode failed\n", chunkId);
        muSendAck("chunk_retry", chunkId);
        muCtx.consecutiveErrors++;
        return;
      }
      
      muCtx.bytesReceived += binLen;
      muCtx.chunksReceived++;
      muCtx.nextChunkExpected = chunkId + 1;
      muCtx.consecutiveErrors = 0;
      
      // 发送当前分片的 ACK
      muSendAck("chunk_ok", chunkId);
      
      // 进度日志(每50个chunk或最后一个)
      if ((chunkId + 1) % 50 == 0 || chunkId + 1 >= muCtx.totalChunks) {
        float pct = (float)muCtx.chunksReceived / muCtx.totalChunks * 100.0f;
        Serial.printf("[MU] Progress: %d/%d chunks (%.1f%%), %zu bytes\n",
                      muCtx.chunksReceived, muCtx.totalChunks, pct, muCtx.bytesReceived);
      }
      
      // ---- 所有分片接收完成 ----
      if (chunkId + 1 >= muCtx.totalChunks || muCtx.bytesReceived >= muCtx.totalSize) {
        muFinalize();
      }
    }
    // ---- 乱序: chunk ID > 期望 → 请求重发期望的chunk ----
    else if (chunkId > muCtx.nextChunkExpected) {
      Serial.printf("[MU] SKIP chunk %d (expected %d), requesting retry\n",
                    chunkId, muCtx.nextChunkExpected);
      muSendAck("chunk_retry", muCtx.nextChunkExpected);
      muCtx.consecutiveErrors++;
      
      // 连续错误过多，终止
      if (muCtx.consecutiveErrors >= 10) {
        Serial.println("[MU] ERR: too many consecutive errors, aborting");
        muSendAck("model_fail", -1);
        muCleanup();
      }
    }
    // ---- 重复包: chunk ID < 期望 → 重发ACK，忽略数据 ----
    else {
      // 已收到过的chunk，只重发ACK
      muSendAck("chunk_ok", chunkId);
    }
    
    return;
  }
  
  // ============================================================
  // Phase 2A: 完整 JSON "model config <name> <minified_json>"
  //           → 解析 min/max/classes，更新内存 + 写入 SD _norm.json
  // ============================================================
  if (cmd.startsWith("model config ")) {
    int p1 = cmd.indexOf(' ', 13);  // 跳过 "model config "
    if (p1 < 0) { muSendAck("model_fail", -1); return; }
    String name = cmd.substring(13, p1);
    String json = cmd.substring(p1 + 1);
    json.trim();
    if (json.length() == 0 || name.length() == 0) { muSendAck("model_fail", -1); return; }
    
    Serial.printf("[MU] Config received for %s, %d bytes\n", name.c_str(), json.length());
    
    bool minParsed = false, maxParsed = false, classesParsed = false;
    
    // --- 解析 "min" ---
    int minPos = json.indexOf("\"min\"");
    if (minPos >= 0) {
      int b1 = json.indexOf("[", minPos);
      int b2 = json.indexOf("]", b1);
      if (b1 >= 0 && b2 > b1) {
        String arr = json.substring(b1 + 1, b2);
        int idx = 0, s = 0;
        while (idx < 16 && s < (int)arr.length()) {
          int c = arr.indexOf(',', s);
          if (c < 0) c = arr.length();
          String v = arr.substring(s, c); v.trim();
          normParams.min[idx] = v.toFloat(); idx++;
          s = c + 1;
        }
        minParsed = true;
      }
    }
    
    // --- 解析 "max" ---
    int maxPos = json.indexOf("\"max\"");
    if (maxPos >= 0) {
      int b1 = json.indexOf("[", maxPos);
      int b2 = json.indexOf("]", b1);
      if (b1 >= 0 && b2 > b1) {
        String arr = json.substring(b1 + 1, b2);
        int idx = 0, s = 0;
        while (idx < 16 && s < (int)arr.length()) {
          int c = arr.indexOf(',', s);
          if (c < 0) c = arr.length();
          String v = arr.substring(s, c); v.trim();
          normParams.max[idx] = v.toFloat(); idx++;
          s = c + 1;
        }
        maxParsed = true;
      }
    }
    
    if (minParsed && maxParsed) {
      normParams.loaded = true;
      Serial.printf("[MU] Norm params loaded (min[0]=%.2f, max[0]=%.2f)\n",
                    normParams.min[0], normParams.max[0]);
    }
    
    // --- 解析 "classes" 或 "class_names" ---
    int classPos = json.indexOf("\"classes\"");
    if (classPos < 0) classPos = json.indexOf("\"class_names\"");
    if (classPos >= 0) {
      int b1 = json.indexOf("[", classPos);
      if (b1 >= 0) {
        int nFound = 0, pos = b1 + 1;
        while (nFound < MAX_CLASSES && pos < json.length()) {
          int q1 = json.indexOf("\"", pos);
          if (q1 < 0) break;
          int q2 = json.indexOf("\"", q1 + 1);
          if (q2 < 0) break;
          String cls = json.substring(q1 + 1, q2); cls.trim();
          if (cls.length() > 0) {
            strncpy(classNamesBuffer[nFound], cls.c_str(), 31);
            classNamesBuffer[nFound][31] = '\0';
            classNames[nFound] = classNamesBuffer[nFound];
            nFound++;
          }
          pos = q2 + 1;
          int nb = json.indexOf("]", q2);
          int nc = json.indexOf(",", q2);
          if (nb > 0 && (nc < 0 || nb < nc)) break;
        }
        if (nFound > 0) {
          numClasses = nFound;
          classesParsed = true;
          Serial.printf("[MU] Classes loaded: %d\n", numClasses);
        }
      }
    }
    
    // ---- 保存到 SD ----
    if (sd_ready && classesParsed) {
      char jsonPath[64];
      snprintf(jsonPath, sizeof(jsonPath), "/%s_norm.json", name.c_str());
      File f = SD.open(jsonPath, FILE_WRITE);
      if (f) {
        f.print(json);
        f.close();
        Serial.printf("[MU] Norm JSON saved: %s\n", jsonPath);
      }
    }
    
    muSendAck("model_done", -1);
    return;
  }
  
  // ============================================================
  // Phase 2B: 分开发送 norm "model norm <name> <min0>,...,<min15> <max0>,...,<max15>"
  // ============================================================
  if (cmd.startsWith("model norm ")) {
    int p1 = cmd.indexOf(' ', 11);  // 跳过 "model norm "
    if (p1 < 0) { muSendAck("model_fail", -1); return; }
    String name = cmd.substring(11, p1);
    
    int p2 = cmd.indexOf(' ', p1 + 1);
    if (p2 < 0) { muSendAck("model_fail", -1); return; }
    String minStr = cmd.substring(p1 + 1, p2);
    String maxStr = cmd.substring(p2 + 1);
    minStr.trim(); maxStr.trim();
    
    // 解析 min 数组
    int idx = 0, s = 0;
    while (idx < 16 && s < (int)minStr.length()) {
      int c = minStr.indexOf(',', s);
      if (c < 0) c = minStr.length();
      String v = minStr.substring(s, c); v.trim();
      normParams.min[idx] = v.toFloat(); idx++;
      s = c + 1;
    }
    
    // 解析 max 数组
    idx = 0; s = 0;
    while (idx < 16 && s < (int)maxStr.length()) {
      int c = maxStr.indexOf(',', s);
      if (c < 0) c = maxStr.length();
      String v = maxStr.substring(s, c); v.trim();
      normParams.max[idx] = v.toFloat(); idx++;
      s = c + 1;
    }
    
    normParams.loaded = true;
    Serial.printf("[MU] Norm params updated for %s (min[0]=%.2f, max[0]=%.2f)\n",
                  name.c_str(), normParams.min[0], normParams.max[0]);
    
    muSendAck("chunk_ok", 0);
    return;
  }
  
  // ============================================================
  // Phase 2C: 仅下发类名 "model classes <name> air,banana_fresh,banana_rotten,banana_stale"
  //           → 不覆盖已有 norm，仅更新 classes 并写入 _norm.json
  // ============================================================
  if (cmd.startsWith("model classes ")) {
    int p1 = cmd.indexOf(' ', 14);
    if (p1 < 0) { muSendAck("model_fail", -1); return; }
    String name = cmd.substring(14, p1);
    String classList = cmd.substring(p1 + 1);
    classList.trim();
    if (classList.length() == 0 || name.length() == 0) { muSendAck("model_fail", -1); return; }
    
    int nFound = 0, startPos = 0;
    while (nFound < MAX_CLASSES && startPos < (int)classList.length()) {
      int nextComma = classList.indexOf(',', startPos);
      if (nextComma < 0) nextComma = classList.length();
      String cls = classList.substring(startPos, nextComma); cls.trim();
      if (cls.length() > 0) {
        strncpy(classNamesBuffer[nFound], cls.c_str(), 31);
        classNamesBuffer[nFound][31] = '\0';
        classNames[nFound] = classNamesBuffer[nFound];
        nFound++;
      }
      startPos = nextComma + 1;
    }
    
    if (nFound > 0) {
      numClasses = nFound;
      Serial.printf("[MU] Classes updated: %d classes for %s\n", numClasses, name.c_str());
      for (int i = 0; i < numClasses && i < 5; i++)
        Serial.printf("  [%d] %s\n", i, classNames[i]);
      if (numClasses > 5) Serial.printf("  ... and %d more\n", numClasses - 5);
    }
    
    if (sd_ready) {
      char jsonPath[64];
      snprintf(jsonPath, sizeof(jsonPath), "/%s_norm.json", name.c_str());
      File f = SD.open(jsonPath, FILE_WRITE);
      if (f) {
        // 保留已有 norm 或用默认值
        f.print("{\"min\":[");
        for (int i = 0; i < 16; i++) { if (i>0) f.print(","); f.print(normParams.loaded ? normParams.min[i] : 0.0f); }
        f.print("],\"max\":[");
        for (int i = 0; i < 16; i++) { if (i>0) f.print(","); f.print(normParams.loaded ? normParams.max[i] : 1.0f); }
        f.print("],\"classes\":[");
        for (int i = 0; i < numClasses; i++) {
          if (i > 0) f.print(",");
          f.printf("\"%s\"", classNames[i]);
        }
        f.print("]}");
        f.close();
        Serial.printf("[MU] Norm JSON saved: %s\n", jsonPath);
      }
    }
    
    muSendAck("model_done", -1);
    return;
  }
  
  // 取消更新 "model cancel"
  // ============================================================
  if ((cmd == "model cancel") &&
      (muCtx.state == MUS_HANDSHAKE || muCtx.state == MUS_RECEIVING)) {
    Serial.println("[MU] Cancelled by P4");
    muSendAck("model_fail", -1);
    muCleanup();
    return;
  }
  
  // 未知命令
  if (muCtx.state != MUS_IDLE) {
    Serial.printf("[MU] WARN: unexpected cmd in state %d: %s\n", muCtx.state, cmd.c_str());
  }
}

// ==================== 超时检测 (loop中调用) ====================
void muCheckTimeout() {
  if (muCtx.state != MUS_RECEIVING && muCtx.state != MUS_HANDSHAKE) return;
  if (muCtx.lastActivityMs == 0) return;
  
  if (millis() - muCtx.lastActivityMs > MODEL_UPDATE_TIMEOUT) {
    Serial.printf("[MU] Timeout (%lu ms) — aborting\n", millis() - muCtx.lastActivityMs);
    muSendAck("model_fail", -1);
    muCleanup();
  }
}

// ==================== 全部接收完成 → 校验 + 写入SD + 热重载 ====================
// Forward declare sd_manager functions
void scanModels();
bool loadModelByIndex(int idx);
bool loadModelByFile(const char* filename);

static void muFinalize() {
  if (muCtx.finalized) return;  // 防重入
  muCtx.finalized = true;
  muCtx.state = MUS_VERIFYING;
  
  Serial.printf("[MU] All %d chunks received, %zu bytes — verifying\n",
                muCtx.chunksReceived, muCtx.bytesReceived);
  
  // ---- 1. 缓冲区校验 ----
  if (!muCtx.buffer || muCtx.bytesReceived == 0) {
    Serial.println("[MU] ERR: empty buffer");
    muSendAck("model_fail", -1);
    muCleanup();
    return;
  }
  
  // 字节数校验: 允许最后一个chunk填充对齐导致的少量冗余
  if (muCtx.bytesReceived > muCtx.totalSize) {
    Serial.printf("[MU] ERR: bytes received (%zu) > total size (%zu)\n",
                  muCtx.bytesReceived, muCtx.totalSize);
    muSendAck("model_fail", -1);
    muCleanup();
    return;
  }
  
  // ---- 2. TFLite Schema 预校验 (写SD前先检查) ----
  const tflite::Model* candidateModel = tflite::GetModel(muCtx.buffer);
  if (candidateModel->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("[MU] ERR: schema mismatch, expected %d got %d\n",
                  TFLITE_SCHEMA_VERSION, candidateModel->version());
    muSendAck("model_fail", -1);
    muCleanup();
    return;
  }
  Serial.println("[MU] Schema check OK");
  
  // ---- 3. SD卡写入 (原子写入: .tmp → rename) ----
  if (!sd_ready) {
    Serial.println("[MU] ERR: SD not ready");
    muSendAck("model_fail", -1);
    muCleanup();
    return;
  }
  
  // 目标文件路径: /{name}.tflite
  char targetPath[64];
  snprintf(targetPath, sizeof(targetPath), "/%s.tflite", muCtx.modelName);
  
  // 临时文件路径: /{name}.tflite.tmp
  char tmpPath[68];
  snprintf(tmpPath, sizeof(tmpPath), "/%s.tflite%s", muCtx.modelName, MODEL_TMP_SUFFIX);
  
  // 备份文件路径: /{name}.tflite.bak
  char bakPath[68];
  snprintf(bakPath, sizeof(bakPath), "/%s.tflite%s", muCtx.modelName, MODEL_BAK_SUFFIX);
  
  // 删除旧临时文件
  if (SD.exists(tmpPath)) {
    SD.remove(tmpPath);
  }
  
  // 写入临时文件 (分块写入，避免大块一次性写入超时)
  File tmpFile = SD.open(tmpPath, FILE_WRITE);
  if (!tmpFile) {
    Serial.printf("[MU] ERR: can't create %s\n", tmpPath);
    muSendAck("model_fail", -1);
    muCleanup();
    return;
  }
  
  // 分块写入: 每次写4KB，避免大模型一次性write()出问题
  const size_t WRITE_BLOCK = 4096;
  size_t totalWritten = 0;
  while (totalWritten < muCtx.bytesReceived) {
    size_t toWrite = muCtx.bytesReceived - totalWritten;
    if (toWrite > WRITE_BLOCK) toWrite = WRITE_BLOCK;
    size_t written = tmpFile.write(muCtx.buffer + totalWritten, toWrite);
    if (written != toWrite) {
      Serial.printf("[MU] ERR: write block failed at offset %zu (%zu != %zu)\n",
                    totalWritten, written, toWrite);
      tmpFile.close();
      SD.remove(tmpPath);
      muSendAck("model_fail", -1);
      muCleanup();
      return;
    }
    totalWritten += written;
  }
  tmpFile.flush();
  tmpFile.close();
  Serial.printf("[MU] Written %zu bytes to %s\n", totalWritten, tmpPath);
  
  // ---- 4. 读回验证 ----
  File verifyFile = SD.open(tmpPath);
  if (verifyFile) {
    size_t fileSize = verifyFile.size();
    verifyFile.close();
    if (fileSize != muCtx.bytesReceived) {
      Serial.printf("[MU] ERR: verify size mismatch (%zu != %zu)\n",
                    fileSize, muCtx.bytesReceived);
      SD.remove(tmpPath);
      muSendAck("model_fail", -1);
      muCleanup();
      return;
    }
  } else {
    Serial.printf("[MU] WARN: can't open %s for verification\n", tmpPath);
    // 不终止，继续部署
  }
  
  // ---- 5. 备份旧模型 + 原子rename ----
  // 如果目标文件已存在，先备份
  if (SD.exists(targetPath)) {
    if (SD.exists(bakPath)) {
      SD.remove(bakPath);
    }
    if (!SD.rename(targetPath, bakPath)) {
      Serial.printf("[MU] WARN: backup rename failed %s → %s\n", targetPath, bakPath);
    } else {
      Serial.printf("[MU] Backup: %s → %s\n", targetPath, bakPath);
    }
  }
  
  // 原子重命名: .tmp → 目标
  if (!SD.rename(tmpPath, targetPath)) {
    // rename失败，尝试先删除目标再rename
    Serial.println("[MU] WARN: rename failed, trying delete+rename...");
    if (SD.exists(targetPath)) SD.remove(targetPath);
    if (!SD.rename(tmpPath, targetPath)) {
      Serial.printf("[MU] ERR: final rename failed %s → %s\n", tmpPath, targetPath);
      SD.remove(tmpPath);
      muSendAck("model_fail", -1);
      muCleanup();
      return;
    }
  }
  Serial.printf("[MU] Model saved: %s (%zu bytes)\n", targetPath, muCtx.bytesReceived);
  
  // ---- 6. 热重载TFLite ----
  muCtx.state = MUS_DEPLOYING;
  Serial.println("[MU] Deploying: hot-reloading model...");
  
  // 重新扫描SD卡模型列表
  scanModels();
  
  // 查找新模型的索引
  int targetIdx = -1;
  for (int i = 0; i < modelCount; i++) {
    if (strcmp(modelList[i].filename, targetPath) == 0) {
      targetIdx = i;
      break;
    }
  }
  
  // 兜底：scanModels()目录缓存可能漏掉刚写入的文件，手动补加
  if (targetIdx < 0 && SD.exists(targetPath)) {
    Serial.printf("[MU] File %s exists but scan missed — adding manually\n", targetPath);
    File f = SD.open(targetPath);
    if (f) {
      if (modelCount < MAX_MODELS) {
        strncpy(modelList[modelCount].filename, targetPath, 31);
        modelList[modelCount].filename[31] = '\0';
        const char* slash = strrchr(targetPath, '/');
        const char* base = slash ? slash + 1 : targetPath;
        strncpy(modelList[modelCount].label, base, 31);
        modelList[modelCount].label[31] = '\0';
        char* dot = strstr(modelList[modelCount].label, ".tflite");
        if (dot) *dot = '\0';
        modelList[modelCount].filesize = f.size();
        targetIdx = modelCount;
        modelCount++;
        Serial.printf("[MU] Manual add OK: [%d] %s (%zu bytes)\n", targetIdx, targetPath, modelList[targetIdx].filesize);
      } else {
        Serial.println("[MU] Model list full, cannot add");
      }
      f.close();
    }
  }
  
  if (targetIdx >= 0 && loadModelByIndex(targetIdx)) {
    model_loaded = true;
    modelReady = true;
    Serial.printf("[MU] Deploy OK: [%d] %s\n", targetIdx, targetPath);
    
    // 加载归一化参数 (sd_manager内部loadModelByIndex已调用)
    // 加载类别 (同上)
    
    muSendAck("model_done", -1);
    pendingModelList = true;  // 上报新模型列表
  } else {
    Serial.println("[MU] ERR: hot reload failed");
    
    // 尝试回滚: 从.bak恢复
    if (SD.exists(bakPath) && !SD.exists(targetPath)) {
      SD.rename(bakPath, targetPath);
      Serial.println("[MU] Rollback: restored from .bak");
      scanModels();
      // 尝试加载回滚后的模型
      for (int i = 0; i < modelCount; i++) {
        if (strcmp(modelList[i].filename, targetPath) == 0) {
          if (loadModelByIndex(i)) {
            model_loaded = true;
            modelReady = true;
          }
          break;
        }
      }
    }
    
    muSendAck("model_fail", -1);
  }
  
  muCtx.state = MUS_IDLE;
  Serial.println("[MU] Deploy complete");
  
  // 清理PSRAM缓冲区
  muCleanup();
}

// ==================== 清理资源 ====================
static void muCleanup() {
  if (muCtx.buffer) {
    free(muCtx.buffer);
    muCtx.buffer = nullptr;
  }
  muCtx.bufferSize = 0;
  muCtx.state = MUS_IDLE;
  muCtx.finalized = false;
  muCtx.nextChunkExpected = 0;
  muCtx.bytesReceived = 0;
  muCtx.chunksReceived = 0;
  muCtx.consecutiveErrors = 0;
}

// ==================== 状态查询 (串口/调试用) ====================
void muPrintStatus() {
  Serial.println("\n===== Model Updater Status =====");
  const char* stateNames[] = {"IDLE", "HANDSHAKE", "RECEIVING", "VERIFYING", "DEPLOYING"};
  if (muCtx.state >= 0 && muCtx.state <= 4) {
    Serial.printf("State:       %s\n", stateNames[muCtx.state]);
  }
  if (muCtx.state != MUS_IDLE) {
    Serial.printf("Model:       %s v%s\n", muCtx.modelName, muCtx.modelVersion);
    Serial.printf("Progress:    %d/%d chunks, %zu/%zu bytes\n",
                  muCtx.chunksReceived, muCtx.totalChunks,
                  muCtx.bytesReceived, muCtx.totalSize);
    Serial.printf("Next chunk:  %d\n", muCtx.nextChunkExpected);
    Serial.printf("Buffer:      %s (%zu bytes)\n",
                  muCtx.buffer ? "allocated" : "null", muCtx.bufferSize);
    if (muCtx.lastActivityMs > 0) {
      Serial.printf("Last active: %lu ms ago\n", millis() - muCtx.lastActivityMs);
    }
  }
  Serial.println("================================");
}

#endif // MODEL_UPDATER_H
