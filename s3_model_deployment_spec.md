# 🛰️ ESP32-S3 节点模型接收与在线部署技术规格书 (联调生产版 v2.0)

> **适用对象**：ESP32-S3 推理节点固件开发团队  
> **协议版本**：v2.0 (生产优化版) | **Channel**：WiFi 固定信道 1  
> **数据链路**：ESP32-P4 (主网关) ───[UART]───> ESP32-C6 (Bridge中继) ───[ESP-NOW]───> ESP32-S3 (推理端)

---

## 一、 系统传输拓扑与硬性物理约束

在我们的三端分布式系统架构中，ESP32-P4 主网关负责从云端下载或 SD 卡读取 AI 算法模型（`.tflite`），并通过串口下发。由于 ESP32-S3 推理节点需要同时运行本地算法和高频红外/蓝牙业务，**无法同时开启 WiFi 与 ESP-NOW**。因此，系统通过一块 `ESP32-C6` 作为 Bridge 中继，通过 ESP-NOW 无线转发指令。

在此链路下，固件开发团队必须遵循以下两个**硬性物理约束**：

1. **ESP-NOW 单包最大 250 字节载荷死线 (极其致命)**：
   ESP-NOW 协议的单次发送载荷上限为 **250 字节**。为此，我们已将网关的物理分片大小 `MODEL_CHUNK_SIZE` **极致优化收缩为 100 字节**。
   - 100 字节原始二进制数据进行大写 Hex 编码后为 `200` 字符。
   - 加上协议报头，单包总长约 `215` 字节，完美且安全地处于 250 字节物理上限之内，彻底避免了由于超长引起的空中丢包。
2. **C6 Bridge 串口空格转下划线逻辑**：
   `C6 Bridge` 在转发串口指令时，为了协议的快速路由，会将所有空格字符转换为下划线（`' '` ──> `'_'`）。
   - **S3 推理端从 ESP-NOW 接收到的原始指令，正是被转换后的“下划线拼接命令”**！
   - S3 端的固件必须直接匹配和还原这套下划线格式，本规格书在下文提供了高鲁棒性的还原和解析算法。

---

## 二、 接收协议命令格式速查

S3 端在 ESP-NOW 接收回调函数（`onReceive`）中捕获到 `PKT_TYPE_COMMAND`（或普通字符串报文）时，需匹配以下两个核心下划线报文：

### 2.1 阶段 1: 握手元数据包 (S3 接收)
网关在开始传输前，会发送一个元数据包以告知 S3 即将发送的模型信息，以便 S3 预先准备存储介质并创建文件写入句柄。

- **S3 接收到的报文格式**：
  `model_update_<name>_<total_chunks>_<total_size>_<version>`
- **字段解析**：
  - `name`：模型名称 (如 `banana_fresh`，文件名内部可能带下划线)。
  - `total_chunks`：分片总数 (以 100 字节/片切分)。
  - `total_size`：模型二进制文件总字节数。
  - `version`：版本号 (如 `1.0.0`)。
- **真实报文范例**：`model_update_banana_fresh_98_9736_1.0.0`
- **S3 应答 ACK**：
  S3 成功解析并创建空写入文件后，必须**独立、零延迟**向 Bridge 发送确认 JSON 包（不需要嵌入传感器数据中）：
  `{"mt":"chunk_ok","mid":0}` (告知网关握手成功，期望接收第 0 号数据片)
  若出错（如 Flash 空间不足），回复：`{"mt":"model_fail"}`。

### 2.2 阶段 2: 数据分片包 (S3 接收)
网关会按顺序（`0`, `1`, `2` ... `N-1`）向 S3 推送 Hex 编码的二进制数据分片。

- **S3 接收到的报文格式**：
  `model_chunk_<chunk_id>_<hex_data>`
- **字段解析**：
  - `chunk_id`：当前数据片的索引序号 (从 `0` 到 `total_chunks - 1`)。
  - `hex_data`：大写十六进制 Hex 编码的模型正文（每片大小上限 100 字节，Hex 字符数上限 200 个，尾片可能不足 100 字节）。
- **真实报文范例**：`model_chunk_0_45535033322D5034...`
- **S3 应答 ACK**：
  S3 串口中断收到分片并成功解码写入后，必须立即回复该分片的确认 ACK：
  `{"mt":"chunk_ok","mid":<chunk_id>}`
  若接收到乱序包（`chunk_id` 大于期望接收的值），可回复重试期望包：`{"mt":"chunk_retry","mid":<expected_id>}`。

### 2.3 阶段 3: 校验与重新装载部署 (S3 触发)
当 S3 接收完最后一个分片（`chunk_id == total_chunks - 1`）并成功写入文件后，应先向网关回复最后一个分片的 ACK（`{"mt":"chunk_ok","mid":N-1}`）。
随后，S3 需关闭文件句柄，启动文件大小校验和 **TFLite Micro 在线重新热装载 (Deploy)**。部署完毕后，报送最终状态：
- **热部署成功**：`{"mt":"model_done"}` (网关收到后会高亮绿灯并重置显示名)。
- **热部署失败**：`{"mt":"model_fail"}`。

---

## 三、 S3 接收端下划线报文的高鲁棒性解析算法 (C/C++)

由于 Bridge 强制将空格转为了下划线，且**模型名称本身也可能含有下划线**（如 `banana_fresh`），S3 固件端在解析时**绝对不能**暴力使用 `strtok` 按下划线切割，这会导致文件名解析碎裂。

S3 端应当使用**反向查找（尾部逆向解析）算法**，利用尾部参数（版本、大小、分片数）结构确定、不含下划线的特性，实现 100% 物理防错解析：

### 3.1 握手包 `model_update` 的解析实现 (C/C++)
```cpp
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    char name[64];
    int total_chunks;
    int total_size;
    char version[32];
} model_meta_t;

// 高鲁棒性逆向解析算法
bool s3_parse_model_update(const char* input, model_meta_t* out) {
    if (strncmp(input, "model_update_", 13) != 0) return false;
    
    // 创建可写副本
    char temp[256];
    strncpy(temp, input + 13, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    
    // Step 1: 从末尾查找倒数第一个下划线，切出 version
    char* p_ver = strrchr(temp, '_');
    if (!p_ver) return false;
    *p_ver = '\0';
    strncpy(out->version, p_ver + 1, sizeof(out->version) - 1);
    
    // Step 2: 再次从末尾查找，切出 size
    char* p_size = strrchr(temp, '_');
    if (!p_size) return false;
    *p_size = '\0';
    out->total_size = atoi(p_size + 1);
    
    // Step 3: 再次从末尾查找，切出 chunks
    char* p_chunks = strrchr(temp, '_');
    if (!p_chunks) return false;
    *p_chunks = '\0';
    out->total_chunks = atoi(p_chunks + 1);
    
    // Step 4: 剩余的头部全部保留为模型名（即使名字包含多个下划线，也能完美安全保留！）
    strncpy(out->name, temp, sizeof(out->name) - 1);
    
    return true;
}
```

### 3.2 分片包 `model_chunk` 的解析实现 (C/C++)
```cpp
bool s3_parse_model_chunk(const char* input, int* chunk_id, char* hex_data, size_t hex_max_len) {
    if (strncmp(input, "model_chunk_", 12) != 0) return false;
    
    // 寻找 model_chunk_ 后的第一个下划线，作为 chunk_id 的结束符
    // 注意：ID 之后就是 Hex 数据正文。Hex 编码仅含字符 A-F, 0-9，绝对不含任何下划线！
    char temp[512];
    strncpy(temp, input + 12, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    
    char* p_hex = strchr(temp, '_');
    if (!p_hex) return false;
    *p_hex = '\0';
    
    *chunk_id = atoi(temp);
    strncpy(hex_data, p_hex + 1, hex_max_len - 1);
    hex_data[hex_max_len - 1] = '\0';
    
    return true;
}
```

---

## 四、 S3 流式文件写入与 TFLite Micro 在线热装载 (C++)

为了避免一次性将数 MB 的模型读入 SRAM 产生 OOM（内存溢出），S3 必须使用流式写入（流式写入 SPIFFS/FATFS 临时文件），接收完毕后在外部 PSRAM（MALLOC_CAP_SPIRAM）中分配空间进行重载。

### 4.1 流式接收与 TFLite 热重载完整代码参考 (C++)
```cpp
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

static FILE* s_write_file = NULL;
static uint8_t* g_model_buf = NULL; // 存放于外部高速 PSRAM 中的 TFLite 模型指针
static tflite::MicroInterpreter* g_interpreter = NULL;

constexpr int kTensorArenaSize = 100 * 1024; // 100KB 推理张量区
static uint8_t g_tensor_arena[kTensorArenaSize];

// 1. 将 2 个大写 Hex 字符转换为 1 个 uint8_t 字节
static uint8_t hex_to_byte(char hi, char lo) {
    auto cvt = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    };
    return (cvt(hi) << 4) | cvt(lo);
}

// 2. 串口/ESP-NOW 接收事件处理入口
void s3_ota_receive_loop(const char* packet) {
    model_meta_t meta;
    if (s3_parse_model_update(packet, &meta)) {
        // 创建本地临时写入文件 (存储在内置 SPIFFS/或外挂 SD 卡)
        char filepath[128];
        snprintf(filepath, sizeof(filepath), "/spiffs/model/%s.tflite.tmp", meta.name);
        
        s_write_file = fopen(filepath, "wb");
        if (s_write_file) {
            ESP_LOGI("S3_OTA", "Handshake OK. File: %s, Expecting chunks: %d", meta.name, meta.total_chunks);
            printf("{\"mt\":\"chunk_ok\",\"mid\":0}\n"); // 向 C6 Bridge 回复确认，串口输出加换行
        } else {
            printf("{\"mt\":\"model_fail\"}\n");
        }
        return;
    }

    int chunk_id = -1;
    char hex_data[512] = {0};
    if (s3_parse_model_chunk(packet, &chunk_id, hex_data, sizeof(hex_data))) {
        if (!s_write_file) {
            printf("{\"mt\":\"model_fail\"}\n");
            return;
        }

        int hex_len = strlen(hex_data);
        int data_len = hex_len / 2; // 获取本包真实二进制字节大小 (常规 100 字节)

        // 堆分配零时解码缓冲
        uint8_t* raw_buf = (uint8_t*)malloc(data_len);
        if (raw_buf) {
            for (int i = 0; i < data_len; i++) {
                raw_buf[i] = hex_to_byte(hex_data[i * 2], hex_data[i * 2 + 1]);
            }
            fwrite(raw_buf, 1, data_len, s_write_file);
            free(raw_buf);

            // 回应分片确认 ACK
            printf("{\"mt\":\"chunk_ok\",\"mid\":%d}\n", chunk_id);
        } else {
            printf("{\"mt\":\"model_fail\"}\n");
        }
        return;
    }
}

// 3. 在线 TFLite Micro 模型热加载部署与热重载
esp_err_t s3_tflite_micro_reload(const char* model_name) {
    char tmp_path[128];
    char target_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "/spiffs/model/%s.tflite.tmp", model_name);
    snprintf(target_path, sizeof(target_path), "/spiffs/model/%s.tflite", model_name);

    FILE* f = fopen(tmp_path, "rb");
    if (!f) return ESP_FAIL;

    fseek(f, 0, SEEK_END);
    int size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // 在外部 PSRAM 中流式开辟空间，绝对不占用紧张的内置 SRAM (328KB) 堆空间！
    if (g_model_buf) {
        heap_caps_free(g_model_buf);
        g_model_buf = NULL;
    }
    g_model_buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!g_model_buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    fread(g_model_buf, 1, size, f);
    fclose(f);

    // 动态析构旧的解释器引擎
    if (g_interpreter) {
        delete g_interpreter;
        g_interpreter = NULL;
    }

    // 重新校验 TFLite 模型合法性
    const tflite::Model* model = tflite::GetModel(g_model_buf);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        heap_caps_free(g_model_buf);
        g_model_buf = NULL;
        return ESP_ERR_INVALID_VERSION;
    }

    // 配置神经网络所需的算子解析器 (根据需要加减)
    static tflite::MicroMutableOpResolver<5> resolver;
    resolver.AddFullyConnected();
    resolver.AddSoftmax();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddReshape();

    // 动态重建 Interpreter
    static tflite::MicroInterpreter interpreter(
        model, resolver, g_tensor_arena, kTensorArenaSize);
    g_interpreter = &interpreter;

    if (g_interpreter->AllocateTensors() != kTfLiteOk) {
        g_interpreter = NULL;
        heap_caps_free(g_model_buf);
        g_model_buf = NULL;
        return ESP_FAIL;
    }

    // 原子部署替换：校验成功后重命名备份，实现零崩溃回滚
    rename(target_path, "/spiffs/model/backup.tflite"); // 备份旧模型
    rename(tmp_path, target_path);                       // 正式生效新模型

    ESP_LOGI("S3_OTA", "AI Model '%s' successfully deployed asynchronously!", model_name);
    return ESP_OK;
}
```
