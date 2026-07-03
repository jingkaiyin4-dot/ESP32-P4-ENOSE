# S3 Receiver 接收端代码报告

> 项目：AIoT果蔬新鲜度监测系统 | 版本：v2.0 | 日期：2026-06-30

---

## 1. 代码总览

- **总行数**：4503行（12个活跃文件 + 2个禁用文件）
- **主入口**：`espnow-receiver-s3.ino` (332行)
- **编译目标**：esp32:esp32:esp32s3 (XIAO_ESP32S3)
- **开发环境**：Arduino IDE 2.x + ESP32 Core v3.x
- **PSRAM配置**：Tools → PSRAM = OPI PSRAM (8MB)

---

## 2. 模块详细分析

### 2.1 config.h — 全局配置中心（222行）

**核心定义**：

| 类别 | 定义 | 值/说明 |
|------|------|---------|
| 设备名 | `DEVICE_NAME` | "S3_03" |
| MAC地址 | `RECEIVER_MAC` | A8:03:2A:E1:00:02 |
| | `SENDER_MAC` | A8:03:2A:E1:00:01 |
| | `BRIDGE_MAC` | A8:03:2A:E1:00:03 |
| SD卡 | `SD_SCK/MISO/MOSI/CS` | 18/14/13/15 |
| 模型 | `MODEL_FILE` | "/model.tflite" |
| | `TENSOR_ARENA_SIZE` | 100000 (100KB) |
| | `MAX_CLASSES` | 20 |
| | `MODEL_UPDATE_MAX_SIZE` | 512KB |
| | `MODEL_CHUNK_SIZE` | 100 (hex编码后) |
| 外设 | `RELAY_PIN` | GPIO1 (UV继电器) |
| | `SERVO_PIN` | GPIO4 (盖子舵机) |
| 状态位 | `STATUS_ADS1115_OK` | 0x01 |
| | `STATUS_MHZ19C_OK` | 0x02 |
| | `STATUS_BME680_OK` | 0x04 |

**extern声明体系**：所有模块的全局变量通过`config.h`的extern声明统一管理，避免跨文件链接问题。

---

### 2.2 espnow-receiver-s3.ino — 主循环（332行）

**setup()流程（L74-172）**：

```
Serial.begin → PSRAM初始化 → initSD → scanModels → loadModelByIndex(0)
→ initESPNow → initRelay → initServo → muInit → initFog → initFan
→ serialTask(Core1) → 等待ESP-NOW数据
```

**loop()核心逻辑（L177-331）**：

| 优先级 | 任务 | 频率 | 说明 |
|--------|------|------|------|
| **P0** | sendToP4() | 每帧 | 上报P4，最快路径 |
| P1 | 本地自动控制 | 每帧 | uvAutoCheck/lidAutoCheck/fogAutoCheck/fanAutoCheck |
| P1 | 串口$DATA输出 | 500ms限流 | 给GUI采集用 |
| P2 | 推理日志 | 500ms限流 | Serial.printf结果 |
| P3 | SD卡日志 | 2秒限流 | 最慢操作，降频 |
| 后台 | 模型重试 | 5秒间隔 | SD/model未就绪时自动重试 |
| 后台 | 模型列表发送 | 状态驱动 | OTA后/切换后触发 |

**关键修改点**：
- L198: `predClass >= 0 && predClass < numClasses` 越界保护（本次新增）
- L221: 三元运算符越界保护（本次新增）

---

### 2.3 ble_manager.h — ESP-NOW管理器（359行）

**核心功能**：

| 函数 | 行号 | 功能 |
|------|------|------|
| `initESPNow()` | L132 | WiFi初始化+ESP-NOW注册+peer添加(Sender/C6) |
| `onDataRecv()` | L74 | ESP-NOW回调→环缓冲写入(ISR安全) |
| `sendToP4()` | L275 | 构建JSON→ESP-NOW发送到C6 Bridge |
| `sendWarmupToP4()` | L184 | 预热状态上报 |
| `triggerModelListSend()` | L196 | 触发模型列表逐帧发送 |
| `modelListSendLoop()` | L207 | 逐帧发送model_detail |

**数据环缓冲**：
- `sensorRing[SENSOR_RING_SIZE]` — ISR写(loop读)，volatile保护
- `ringHead/ringTail/ringCount` — 环形队列索引
- 回调中只做memcpy+更新索引，零延迟

**sendToP4()修改**（L275-282）：
```cpp
// 越界保护：默认 "unknown"，仅有效时取真实类名
const char* mainClassName = "unknown";
if (model_loaded && predClass >= 0 && predClass < numClasses) {
    mainClassName = classNames[predClass];
}
```

---

### 2.4 sd_manager.h — SD卡+模型管理（601行）

**核心功能**：

| 函数 | 行号 | 功能 |
|------|------|------|
| `initSD()` | L40 | SPIClass初始化+SD.begin |
| `scanModels()` | L69 | 扫描SD根目录.tflite文件 |
| `loadModelByFile()` | L210 | 加载TFLite模型+PSRAM缓冲+interpreter |
| `loadNormParams()` | L310+ | 加载norm.json归一化参数 |
| `initDefaultClasses()` | L15 | 默认13类名初始化 |
| `listModels()` | L147 | 打印模型列表 |
| `logSensorDataToSD()` | L400+ | CSV日志写入 |
| `csvRotateCheck()` | L500+ | 日志文件旋转 |

**initSD()当前实现**（L40-64）：
```cpp
sd_spi = new SPIClass();  // 默认HSPI(SPI3)
sd_spi->begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);  // SCK=18,MISO=14,MOSI=13,CS=15
SD.begin(SD_CS, *sd_spi);
```

**loadModelByFile()安全检查**（L284-306，本次新增）：
```cpp
int modelOutputClasses = output_tensor->dims->data[output_tensor->dims->size - 1];
if (numClasses != modelOutputClasses) {
    if (numClasses > modelOutputClasses) numClasses = modelOutputClasses;  // 截断
    else if (numClasses < modelOutputClasses) {  // 补齐
        for (int i = numClasses; i < modelOutputClasses && i < MAX_CLASSES; i++) {
            snprintf(classNamesBuffer[i], 32, "class_%d", i);
            classNames[i] = classNamesBuffer[i];
        }
        numClasses = modelOutputClasses;
    }
}
Serial.printf("[CLASSES] Final: %d classes:\n", numClasses);
for (int i = 0; i < numClasses; i++) Serial.printf("  [%d] %s\n", i, classNames[i]);
```

**默认13类名**（L36-43）：
```
air, apple_fresh, apple_rotten, apple_stale,
banana_fresh, banana_rotten, banana_stale,
chocolate_fresh, chocolate_rotten, chocolate_stale,
general_fresh, general_rotten, general_stale
```

**norm.json加载流程**：
- 从SD卡查找与模型同名的`_norm.json`文件
- 解析`class_names`字段→覆盖默认类名
- 解析`min/max`数组→设置归一化参数
- 失败时回退默认13类

---

### 2.5 model_runner.h — TFLite推理引擎（200行）

**推理流程**（`runInference()` L71-128）：

1. 提取16维特征：Base5(Odor/HCHO/CO/VOC/CO2) → Ratio6 → Norm5
2. 归一化：`(x - min) / (max - min)` 使用normParams
3. 填充interpreter input tensor
4. `interpreter->Invoke()` 执行推理
5. 读取output tensor → argmax → 返回predClass

**新鲜度评分**（`calculateFreshnessScore()` L174）：
- 空气类 → 固定50分
- 新鲜类 → confidence×100
- 变质类 → (1-confidence)×60
- 腐烂类 → (1-confidence)×30

**空气检测**（`isAirClass()` L160）：
- 类名包含"air" → 空气类

---

### 2.6 model_updater.h — OTA模型更新器（771行）

**5态状态机**：

| 状态 | 说明 |
|------|------|
| MU_IDLE | 空闲，等待model update命令 |
| MU_RECEIVING | 接收chunk分片，PSRAM缓冲 |
| MU_FINALIZING | 全部chunk收齐，写入SD卡 |
| MU_RELOADING | 重新加载模型 |
| MU_ERROR | 错误，清理资源 |

**PSRAM缓冲策略**：
- 全量缓冲到PSRAM（不边收边写SD）
- 收齐后 `.tmp` → atomic rename → 安全替换
- 最大512KB模型支持

**ACK机制**：
- 每个chunk收到后独立发送ACK（含chunkId+progress%）
- 不依赖sendToP4的帧周期

---

### 2.7 relay_control.h — UV继电器控制（669行）

**核心逻辑**：
- GPIO1 NC型继电器：LOW=通电(UV亮)，HIGH=断电(UV灭)
- 自动模式：空气→水果切换时自动开UV 30秒
- P4心跳检测：从sensor_data.json timestamp推算（2分钟内=在线）
- 下行命令：`uv_on/uv_off/uv_auto/uv_duration/uv_threshold`

---

### 2.8 servo_control.h — 盖子舵机（133行）

- GPIO4 PWM驱动
- 关盖500μs / 开盖2450μs
- 自动模式：新鲜度≤40%→开盖换气60秒后关盖

### 2.9 atomization.h — 雾化加湿（93行）

- GPIO3 脉冲触发模式（按键式雾化器）
- 自动模式：湿度<40%→触发雾化脉冲

### 2.10 fan_control.h — 风扇排风（89行）

- GPIO5 + 3V继电器（12V风扇）
- 自动模式：新鲜度≤50%→自动排风

### 2.11 serial_cmd.h — 串口命令（246行）

**支持的命令**：

| 命令 | 功能 |
|------|------|
| `status` | 打印系统状态 |
| `models` | 打印模型列表 |
| `load <idx>` | 加载指定模型 |
| `unload` | 释放模型 |
| `uv on/off/auto` | UV控制 |
| `lid open/close/auto` | 盖子控制 |
| `fog on/off/auto` | 雾化控制 |
| `fan on/off/auto` | 风扇控制 |
| `model update ...` | 触发OTA |
| `model chunk ...` | 接收OTA分片 |

---

### 2.12 禁用模块

| 模块 | 行数 | 禁用原因 |
|------|------|----------|
| `display_manager.h` | 514 | GPIO18(SCK)同时给TFT和SD卡，SPI冲突 |
| `web_server.h` | 274 | WiFi OTA改为ESP-NOW OTA，不再需要HTTP |

---

## 3. 数据流分析

### 3.1 上行数据流（Sender→P4→云端）

```
Sender每2秒采样 → ESP-NOW → Receiver onDataRecv(ISR)
→ ringBuffer → loop消费 → runInference(16维→TFLite→predClass)
→ sendToP4(JSON) → ESP-NOW → C6 Bridge → UART → P4 → HTTP → 云端
```

**JSON格式**（sendToP4输出）：
```json
{"name":"S3_03","t":28.5,"h":55.0,"odor":0.0,"hcho":0.0,"co":0.0,"voc":0.0,"co2":450,
 "cls":"unknown","fr":50,"mr":1,"ts":12345678}
```

### 3.2 下行命令流（P4→Receiver）

```
P4 HTTP poll → 云端命令 → P4 UART → C6 Bridge ESP-NOW → Receiver onDataRecv
→ p4CmdBuffer → loop → handleP4UvCommand → GPIO动作
```

**命令格式**：`CMD:S3_03:uv_on`

---

## 4. 内存使用分析

| 区域 | 大小 | 用途 |
|------|------|------|
| TFLite Arena | 100KB | interpreter工作内存 |
| 模型缓冲 | ≤512KB | PSRAM OTA缓冲 |
| 环缓冲 | ~2KB | SensorData[8] |
| CSV日志 | 流式 | SD卡写入 |
| 串口任务栈 | 4KB | Core1独立任务 |

**PSRAM使用**：模型文件加载到PSRAM（`model_buffer`），TFLite arena在内部RAM。

---

## 5. 编译配置

| 配置项 | 值 |
|--------|-----|
| Board | XIAO_ESP32S3 |
| PSRAM | OPI PSRAM |
| Flash | 16MB |
| Partition | Default 4MB app |
| Upload Speed | 921600 |
| USB CDC On Boot | Enabled |

---

## 6. 修改历史

| 日期 | 版本 | 修改 | 说明 |
|------|------|------|------|
| 06-02 | v2.0 model_updater | ACK独立+PSRAM缓冲+5态机 | OTA模型更新 |
| 06-03 | +雾化+风扇 | atomization.h+fan_control.h | 新增外设 |
| 06-25 | config.h | SD_SCK=18统一 | 修复引脚定义 |
| 06-26 | ble_manager | 上行JSON加`name`字段 | C6识别设备名 |
| 06-30 | sd_manager | 类名维度安全检查 | 截断/补齐+诊断日志 |
| 06-30 | ble_manager | sendToP4越界保护 | 默认"unknown" |
| 06-30 | .ino | 两处越界保护 | predClass范围检查 |

---

*报告生成：2026-06-30 | 作者：AI工程师*
