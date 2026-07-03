/**
 * sd_manager.h — SD卡、模型管理、数据记录、归一化参数、类别管理
 */
#ifndef SD_MANAGER_H
#define SD_MANAGER_H

#include "config.h"
#include "model_runner.h"
#include <time.h>

// 前向声明
bool loadModelByFile(const char* filename);
void loadNormParams(const char* modelPath);
bool loadClassesFromJson(const char* jsonPath);
void initDefaultClasses(int nClasses);

// ==================== SD 全局 ====================
SPIClass* sd_spi   = nullptr;
bool      sd_ready = false;
File      data_file;

// ==================== 模型列表全局 ====================
ModelInfo modelList[MAX_MODELS];
int       modelCount         = 0;
int       currentModelIndex  = 0;
char      currentModelFile[32] = "/model.tflite";

// ==================== 归一化参数全局 ====================
NormParams normParams = { { 0 }, { 0 }, false };

// ==================== 类别管理全局 ====================
const char* classNames[MAX_CLASSES];
int         numClasses = 0;
char        classNamesBuffer[MAX_CLASSES][32];

// 默认类别: 模型加载时若 JSON 无 classes 字段，根据模型输出维度自动生成 class_0, class_1, ...
// 不再硬编码食材名称，所有类名统一从 SD 卡 _norm.json 读取

// ==================== SD 卡初始化 ====================
bool initSD() {
  if (sd_spi != nullptr) {
    sd_spi->end();
    delete sd_spi;
    sd_spi = nullptr;
  }
  sd_spi = new SPIClass();
  sd_spi->begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  if (!SD.begin(SD_CS, *sd_spi)) {
    sd_ready = false;
    delete sd_spi;
    sd_spi = nullptr;
    return false;
  }
  sd_ready = true;
  return true;
}

// ==================== 判断字符串是否以指定后缀结尾 ====================
bool endsWith(const char* str, const char* suffix) {
  int slen = strlen(str);
  int xlen = strlen(suffix);
  if (xlen > slen) return false;
  return strcmp(str + slen - xlen, suffix) == 0;
}

// ==================== 扫描 SD 卡上的所有 .tflite 模型文件 ====================
void scanModels() {
  modelCount = 0;

  File root = SD.open("/");
  if (!root) {
    Serial.println("[SCAN] Cannot open SD root");
    return;
  }

  Serial.println("[SCAN] Scanning root directory...");

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;

    if (entry.isDirectory()) continue;

    String nameStr = String(entry.name());
    nameStr.trim();

    Serial.printf("[SCAN] File: \"%s\"  size=%zu\n", nameStr.c_str(), entry.size());

    if (!endsWith(nameStr.c_str(), ".tflite")) {
      entry.close();
      continue;
    }

    if (modelCount >= MAX_MODELS) {
      entry.close();
      break;
    }

    String fullPath;
    if (nameStr.startsWith("/")) {
      fullPath = nameStr;
    } else {
      fullPath = "/" + nameStr;
    }

    int lastSlash = fullPath.lastIndexOf('/');
    int lastDot   = fullPath.lastIndexOf('.');
    String label  = fullPath.substring(lastSlash + 1, lastDot);
    if (label.length() == 0) label = fullPath;

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
  scanModels();
}

// ==================== 列出所有模型 ====================
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

// ==================== 按索引加载模型 ====================
bool loadModelByIndex(int index) {
  if (index < 0 || index >= modelCount) {
    Serial.printf("Invalid model index: %d (0-%d)\n", index, modelCount - 1);
    return false;
  }
  return loadModelByFile(modelList[index].filename);
}

// ==================== 按文件名加载模型（核心） ====================
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

  tflite_model = GetModel(model_buffer);
  if (tflite_model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("Schema version mismatch");
    free(model_buffer);
    model_buffer = nullptr;
    tflite_model = nullptr;
    return false;
  }

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

  strncpy(currentModelFile, filename, 31);
  currentModelFile[31] = '\0';
  for (int i = 0; i < modelCount; i++) {
    if (strcmp(modelList[i].filename, filename) == 0) {
      currentModelIndex = i;
      break;
    }
  }

  Serial.printf("Model loaded OK: %s\n", filename);

  TfLiteTensor* input_tensor  = interpreter->input(0);
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

  int modelOutputClasses = output_tensor->dims->data[output_tensor->dims->size - 1];
  Serial.printf("  Input size: %d\n", input_tensor->dims->data[1]);
  Serial.printf("  Output classes: %d\n", modelOutputClasses);
  Serial.printf("  Free heap: %d bytes\n", ESP.getFreeHeap());

  loadNormParams(filename);

  // ---- 如果 JSON 没有提供类名，用模型输出维度自动生成占位名 ----
  if (numClasses == 0) {
    int modelOutputClasses = interpreter->output(0)->dims->data[interpreter->output(0)->dims->size - 1];
    initDefaultClasses(modelOutputClasses);
  }

  // ---- 安全检查：模型输出维度 vs 类名数量 ----
  if (numClasses != modelOutputClasses) {
    Serial.printf("[WARN] Model outputs %d classes but numClasses=%d\n", modelOutputClasses, numClasses);
    if (numClasses > modelOutputClasses) {
      // 类名比模型多（通常是默认13类回退），截断到模型实际输出数
      numClasses = modelOutputClasses;
      Serial.printf("[FIX] numClasses truncated to %d (model output)\n", numClasses);
    } else if (numClasses < modelOutputClasses) {
      // 类名比模型少，补齐占位
      for (int i = numClasses; i < modelOutputClasses && i < MAX_CLASSES; i++) {
        snprintf(classNamesBuffer[i], 32, "class_%d", i);
        classNames[i] = classNamesBuffer[i];
      }
      numClasses = modelOutputClasses;
      Serial.printf("[FIX] numClasses expanded to %d (with placeholder names)\n", numClasses);
    }
  }

  // 打印最终类名列表
  Serial.printf("[CLASSES] Final: %d classes:\n", numClasses);
  for (int i = 0; i < numClasses; i++) {
    Serial.printf("  [%d] %s\n", i, classNames[i]);
  }

  return true;
}

// ==================== 加载模型（兼容旧接口） ====================
bool loadModelFromSD() {
  return loadModelByFile(MODEL_FILE);
}

// ==================== 保存模型到 SD 卡 ====================
bool saveModelToSD(uint8_t* data, size_t size) {
  if (!sd_ready) return false;

  if (SD.exists(MODEL_FILE)) {
    SD.remove(MODEL_FILE);
    Serial.println("Old model removed");
  }

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

// ==================== 记录数据到 SD 卡 ====================
void logSensorDataToSD(const SensorData& data, int pred_class, float freshness) {
  if (!sd_ready || !data_file) return;
  data_file.printf("%u,%.2f,%.2f,%.2f,%.2f,%u,%d,%.2f,%.2f,%u,%d,%.2f\n",
                   data.timestamp, data.odor_ppm, data.hcho_ppm, data.co_ppm, data.voc_ppm,
                   data.co2_ppm, data.co2_temp, data.env_temp, data.humidity,
                   data.sensor_status, pred_class, freshness);
  if (data_file.position() > 4096) data_file.flush();
}

// ==================== CSV 10分钟轮转 ====================
static unsigned long csvRotateLast = 0;
#define CSV_ROTATE_MS  600000  // 10 min

void csvRotateCheck() {
  unsigned long now = millis();
  if (csvRotateLast == 0) csvRotateLast = now;
  if (now - csvRotateLast < CSV_ROTATE_MS) return;
  csvRotateLast = now;

  if (!sd_ready) return;

  // 关闭当前文件
  if (data_file) {
    data_file.flush();
    data_file.close();
  }

  // 打开新文件: /data/sensor_YYYYMMDD_HHMM.csv
  char filename[64];
  time_t unixTime = time(NULL);
  if (unixTime > 1000000000) {
    struct tm* tm = gmtime(&unixTime);
    snprintf(filename, sizeof(filename),
             "/data/sensor_%04d%02d%02d_%02d%02d.csv",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min);
  } else {
    snprintf(filename, sizeof(filename),
             "/data/sensor_%lu.csv", (unsigned long)now);
  }

  SD.mkdir("/data");
  data_file = SD.open(filename, FILE_WRITE);
  if (data_file) {
    data_file.println("timestamp,odor,hcho,co,voc,co2,co2_temp,env_temp,humidity,status,pred,freshness");
    data_file.flush();
    Serial.printf("[CSV] Rotated -> %s\n", filename);
  } else {
    Serial.println("[CSV] Rotate FAILED: can't open new file");
  }
}

// ==================== 归一化参数加载 ====================
void loadNormParams(const char* modelPath) {
  normParams.loaded = false;

  char jsonPath[64];
  strncpy(jsonPath, modelPath, 63);
  jsonPath[63] = '\0';

  char* dot = strstr(jsonPath, ".tflite");
  if (!dot) return;

  strcpy(dot, "_norm.json");
  if (!SD.exists(jsonPath)) {
    strcpy(jsonPath, "/norm.json");
    if (!SD.exists(jsonPath)) {
      Serial.println("[NORM] No norm file found (tried model_norm.json, norm.json)");
      loadClassesFromJson("/norm.json");
      return;
    }
  }

  File f = SD.open(jsonPath);
  if (!f) return;

  // 使用预分配缓冲区一次性读取，避免逐字符 String 拼接的堆分配阻塞
  size_t fSize = f.size();
  char* fileBuf = nullptr;
  if (fSize > 0 && fSize < 16384) {
    fileBuf = (char*)malloc(fSize + 1);
  }
  if (fileBuf) {
    size_t bytesRead = f.read((uint8_t*)fileBuf, fSize);
    fileBuf[bytesRead] = '\0';
    f.close();
  } else {
    // fallback: 逐字符读取（慢但可靠）
    String content = "";
    while (f.available()) content += (char)f.read();
    f.close();
    fileBuf = (char*)malloc(content.length() + 1);
    if (fileBuf) strcpy(fileBuf, content.c_str());
    else { Serial.println("[NORM] malloc failed"); return; }
  }
  String content = String(fileBuf);
  free(fileBuf);

  // --- 解析 min 数组 ---
  int p = content.indexOf("\"min\"");
  if (p < 0) { Serial.println("[NORM] No 'min' field"); return; }
  // 找到 min 数组的左方括号
  int bracketStart = content.indexOf("[", p);
  if (bracketStart < 0) return;
  int bracketEnd = content.indexOf("]", bracketStart);
  if (bracketEnd < 0) return;
  // 提取数组内容（不含方括号）
  String minStr = content.substring(bracketStart + 1, bracketEnd);
  // 按逗号分割解析
  int idx = 0, startPos = 0;
  while (idx < 16 && startPos < (int)minStr.length()) {
    int nextComma = minStr.indexOf(",", startPos);
    if (nextComma < 0) nextComma = minStr.length();
    String val = minStr.substring(startPos, nextComma);
    val.trim();
    normParams.min[idx] = val.toFloat();
    idx++;
    startPos = nextComma + 1;
  }

  // --- 解析 max 数组 ---
  p = content.indexOf("\"max\"");
  if (p < 0) { Serial.println("[NORM] No 'max' field"); return; }
  bracketStart = content.indexOf("[", p);
  if (bracketStart < 0) return;
  bracketEnd = content.indexOf("]", bracketStart);
  if (bracketEnd < 0) return;
  String maxStr = content.substring(bracketStart + 1, bracketEnd);
  idx = 0; startPos = 0;
  while (idx < 16 && startPos < (int)maxStr.length()) {
    int nextComma = maxStr.indexOf(",", startPos);
    if (nextComma < 0) nextComma = maxStr.length();
    String val = maxStr.substring(startPos, nextComma);
    val.trim();
    normParams.max[idx] = val.toFloat();
    idx++;
    startPos = nextComma + 1;
  }

  normParams.loaded = true;
  Serial.printf("[NORM] Loaded 16-dim params\n");
  Serial.printf("[NORM] min[0..4]=%.2f,%.2f,%.2f,%.2f,%.0f\n",
                normParams.min[0], normParams.min[1], normParams.min[2], normParams.min[3], normParams.min[4]);
  Serial.printf("[NORM] min[5..10]=%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                normParams.min[5], normParams.min[6], normParams.min[7], normParams.min[8], normParams.min[9], normParams.min[10]);
  Serial.printf("[NORM] min[11..15]=%.4f,%.4f,%.4f,%.4f,%.4f\n",
                normParams.min[11], normParams.min[12], normParams.min[13], normParams.min[14], normParams.min[15]);

  loadClassesFromJson(jsonPath);
}

// ==================== 动态类别加载 ====================

void initDefaultClasses(int nClasses) {
  nClasses = constrain(nClasses, 1, MAX_CLASSES);
  numClasses = nClasses;
  for (int i = 0; i < numClasses; i++) {
    snprintf(classNamesBuffer[i], 31, "class_%d", i);
    classNamesBuffer[i][31] = '\0';
    classNames[i] = classNamesBuffer[i];
  }
  Serial.printf("[CLASSES] Default fallback: %d generic classes (class_0..class_%d)\n", numClasses, numClasses - 1);
}

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

  // 预分配缓冲区一次性读取
  size_t fSize = f.size();
  char* fileBuf = nullptr;
  if (fSize > 0 && fSize < 16384) {
    fileBuf = (char*)malloc(fSize + 1);
  }
  String content;
  if (fileBuf) {
    size_t bytesRead = f.read((uint8_t*)fileBuf, fSize);
    fileBuf[bytesRead] = '\0';
    f.close();
    content = String(fileBuf);
    free(fileBuf);
  } else {
    // fallback
    while (f.available()) content += (char)f.read();
    f.close();
  }

  int classesPos = content.indexOf("\"classes\"");
  if (classesPos < 0) {
    // 尝试 class_names 字段（新版 norm.json 格式）
    classesPos = content.indexOf("\"class_names\"");
  }
  if (classesPos < 0) {
    Serial.println("[CLASSES] No 'classes' or 'class_names' field in JSON");
    return false;
  }

  int bracketOpen = content.indexOf("[", classesPos);
  if (bracketOpen < 0) {
    Serial.println("[CLASSES] No array start bracket");
    return false;
  }

  int count = 0;
  int pos   = bracketOpen + 1;

  while (count < MAX_CLASSES && pos < content.length()) {
    int quote1 = content.indexOf("\"", pos);
    if (quote1 < 0) break;

    int quote2 = content.indexOf("\"", quote1 + 1);
    if (quote2 < 0) break;

    String className = content.substring(quote1 + 1, quote2);
    className.trim();

    if (className.length() > 0) {
      strncpy(classNamesBuffer[count], className.c_str(), 31);
      classNamesBuffer[count][31] = '\0';
      classNames[count] = classNamesBuffer[count];
      count++;
    }

    pos = quote2 + 1;

    int nextBracket = content.indexOf("]", quote2);
    int nextComma   = content.indexOf(",", quote2);
    if (nextBracket > 0 && (nextComma < 0 || nextBracket < nextComma)) {
      break;
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

#endif
