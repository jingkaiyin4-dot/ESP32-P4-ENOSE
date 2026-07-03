# 🛸 ESP32-P4 网关云端对接与 OTA 模型训练使用指南 (服务端/云端专供)

本指南专为云端及服务器开发队友编写，详细说明了 ESP32-P4 网关与服务器基于 **v3.0 通信协议** 的对接规范，涵盖 WebSocket 长连接、HTTP REST API 端点、以及双向控制数据流。

---

## 🌐 1. WebSocket 长连接协议 (`/p4/ws`)

由于网关需接收云端的主动推送（如实时控制指令、模型训练进度等），网关与云端维持一条长连接通道。

* **WebSocket 连接 URL**:
  ```
  ws://*.*.*.*/p4/ws <!-- Censored Server API / 已脱敏服务器API -->
  ```
* **WebSocket 连接 URL (直连后端 8000 端口，用于开发测试)**:
  ```
  ws://*.*.*.*:8000/p4/ws <!-- Censored Server API / 已脱敏服务器API -->
  ```

### 1.1 握手参数规范 (Query String)
建立 WebSocket 升级请求时，客户端必须在 URL 参数中附带设备识别信息：
* `device_id`: 网关设备唯一 MAC 地址标识 (如 `esp32-240ac4xxxxxx`)
* `device_type`: 固定为 `esp32_p4`
* `firmware_version`: 固件版本，当前为 `1.0.0`

**示例链接**:
```
ws://*.*.*.*/p4/ws?device_id=esp32-240ac4112233&device_type=esp32_p4&firmware_version=1.0.0 <!-- Censored Server API / 已脱敏服务器API -->
```

### 1.2 心跳保活 (Ping/Pong)
* 客户端每 30 秒发送 Ping 文本帧或标准的 WebSocket Ping 帧。
* 服务端接收后必须响应对应的 Pong 帧。
* 若连续 60 秒无心跳，网关将断开并执行指数退避重连。

---

## 🔌 2. HTTP REST API 端点 (REST API Endpoints)

所有的 HTTP 服务均运行在 **80 端口** 下：

| 功能 | 请求方式 | URL 路径 | 请求头 / 参数 | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| **最新模型查询** | `GET` | `/p4/model/latest` | `current_model`: 当前激活模型名 | 网关查询云端是否有比当前加载模型更新的版本 |
| **可用模型列表** | `GET` | `/p4/model/list` | 无 | 获取当前云端数据库中已注册模型列表 |
| **发起模型训练** | `POST` | `/p4/train/request` | Content-Type: `application/json` | 请求云端对指定数据集执行模型自动训练 |
| **模型文件下载** | `GET` | `/model/download/{name}/model.tflite` | 无 | 静态下载 `.tflite` 推理模型二进制文件 |
| **传感器数据上报** | `POST` | `/sensor/data` | `X-API-Key: bigboss` | 网关定期将节点传感器遥测数据上传至云端数据库 |

---

## 🔄 3. 典型双向数据流对接 (Data Flows)

### 3.1 传感器数据上报数据结构 (`POST /sensor/data`)
* **Headers**:
  ```http
  Content-Type: application/json
  X-API-Key: bigboss
  ```
* **JSON 请求体格式**:
  ```json
  {
    "t": 25.5,
    "hu": 60.2,
    "co2": 450,
    "o": 4.5,
    "h": 0.02,
    "c": 15.3,
    "v": 0.8,
    "cls": "air",
    "fr": 95,
    "uv_on": true,
    "fog_on": false,
    "fan_on": true,
    "lid_on": false
  }
  ```

### 3.2 训练请求与进度推送闭环 (`POST /p4/train/request`)
1. **网关或后台发起训练**:
   `POST /p4/train/request`
   ```json
   {
     "csv_files": ["sensor_data.csv"],
     "model_name": "p4_custom_model",
     "target_accuracy": 85
   }
   ```
   *云端应答*: `{"status":"accepted","training_id":"train_20260607_001"}`。
   
2. **云端推送训练进度 (WebSocket)**:
   在训练过程中，服务端应以每 2 秒一次的频率，通过已建立的 WebSocket 长连接向网关推送进度事件通知：
   ```json
   {
     "msg_id": "progress_001",
     "type": "event",
     "timestamp": 1686123456789,
     "data": {
       "event": "training_progress",
       "training_id": "train_20260607_001",
       "status": "training",
       "progress": 45,
       "current_epoch": 45,
       "total_epochs": 100,
       "current_accuracy": 0.82
     }
   }
   ```
   *网关收到后将在屏幕实时更新进度条。*

3. **云端推送训练成功及模型下载地址 (WebSocket)**:
   训练完成后，服务端推送 `training_complete` 事件：
   ```json
   {
     "msg_id": "complete_001",
     "type": "event",
     "timestamp": 1686123456900,
     "data": {
       "event": "training_complete",
       "training_id": "train_20260607_001",
       "status": "success",
       "final_accuracy": 0.94,
       "model_name": "p4_custom_model",
       "download_url": "/model/download/p4_custom_model/model.tflite"
     }
   }
   ```
   *网关收到后，将自动发起 HTTP 下载，并将模型热加载保存至网关的本地 SD 卡中。*

### 3.3 云端主动推送模型更新 (`model_update` 命令)
当云端有发布的新模型，或者需要强制更新某台设备的运行模型时，可通过 WebSocket 发送 `command` 消息：
```json
{
  "msg_id": "push_001",
  "type": "command",
  "timestamp": 1686123456789,
  "data": {
    "action": "model_update",
    "model_name": "fruit_model_v2",
    "version": "2.0.0",
    "download_url": "/model/download/fruit_model_v2/model.tflite",
    "priority": "normal",
    "auto_install": false
  }
}
```
*网关接收到后，会自动发送 `accepted` 响应报文回执并启动下载流程。*

---

## 🛠️ 4. 简易测试命令

* **查询模型列表 (GET)**:
  ```bash
  curl "http://*.*.*.*/p4/model/list" <!-- Censored Server API / 已脱敏服务器API -->
  ```
* **手动测试上报数据 (POST)**:
  ```bash
  curl -X POST "http://*.*.*.*/sensor/data" \ <!-- Censored Server API / 已脱敏服务器API -->
       -H "X-API-Key: ***" \
       -H "Content-Type: application/json" \
       -d '{"t":25.5,"hu":60.2,"co2":450,"o":4.5,"h":0.02,"c":15.3,"v":0.8,"cls":"air","fr":95}'
  ```

---

## 🔄 5. Cloud Sync 轮询控制与 S3 模型管理 (Cloud Sync Polling & S3 Model Management)

为了实现服务器 AI 对网关外设的异步控制，以及对 S3 接收端运行模型的管理，网关实现了 **Cloud Sync 轮询机制 (Cloud Sync Polling)**。

### 5.1 轮询工作流说明 (Workflow Overview)
1. **指令获取 (Command Polling)**:
   网关定期 (如每 5 秒) 向服务器发送 HTTP GET 轮询请求：
   - **URL**: `GET http://*.*.*.*/api/p4/poll?key=***` <!-- Censored Server API / 已脱敏服务器API -->
   - **响应格式 (Response)**:
     - 若无挂起指令，返回：`{"id": null}`
     - 若有指令，返回包含 action、params 和 id 的 JSON：
       ```json
       {
         "id": "unique_cmd_id_123",
         "action": "<action_name>",
         "params": {
           "<param_key>": "<param_value>"
         }
       }
       ```
2. **指令执行与确认 (Ack Mechanism)**:
   网关执行指令后，会将执行状态通过 HTTP POST 提交至服务器以清除队列：
   - **URL**: `POST http://*.*.*.*/api/p4/ack?key=***` <!-- Censored Server API / 已脱敏服务器API -->
   - **请求体格式 (JSON Payload)**:
     ```json
     {
       "id": "unique_cmd_id_123",
       "accept": true,
       "reason": "Execution details or error reasons",
       "result": {
         "<result_key>": "<result_value>"
       }
     }
     ```

---

### 5.2 AI 外设控制指令集 (Peripheral Control Command Set)

服务器可通过 poll 接口返回以下指令控制网关外设：

#### 5.2.1 紫光灯控制 (UV Lamp Control)
* **开启/关闭 (Turn On/Off)**:
  - **Action**: `set_uv`
  - **Params**: `{"on": true}` 或 `{"on": false}`
  - **ACK Result**: `{"uv_on": true/false}`
* **单次工作时长 (Work Duration)**:
  - **Action**: `uv_dur`
  - **Params**: `{"duration": 30}` (单位：分钟)
  - **说明**: 设定单次开启的工作时长，网关会自动换算成秒发送给 S3。
* **自动模式开关 (Auto Mode)**:
  - **Action**: `uv_auto_on` / `uv_auto_off`
  - **Params**: 无

#### 5.2.2 加湿器/水箱控制 (Humidifier/Fogger Control)
* **开启/关闭 (Turn On/Off)**:
  - **Action**: `set_fog`
  - **Params**: `{"on": true}` 或 `{"on": false}`
  - **ACK Result**: `{"fog_on": true/false}`
* **单次工作时长 (Work Duration)**:
  - **Action**: `fog_dur`
  - **Params**: `{"duration": 10}` (单位：分钟)
* **自动模式开关 (Auto Mode)**:
  - **Action**: `fog_auto_on` / `fog_auto_off`
  - **Params**: 无

#### 5.2.3 风扇控制 (Fan Control)
* **开启/关闭 (Turn On/Off)**:
  - **Action**: `set_fan`
  - **Params**: `{"on": true}` 或 `{"on": false}`
  - **ACK Result**: `{"fan_on": true/false}`
* **单次工作时长 (Work Duration)**:
  - **Action**: `fan_dur`
  - **Params**: `{"duration": 15}` (单位：分钟)
* **自动模式开关 (Auto Mode)**:
  - **Action**: `fan_auto_on` / `fan_auto_off`
  - **Params**: 无

#### 5.2.4 舱盖控制 (Lid Control)
* **开启/关闭 (Turn On/Off)**:
  - **Action**: `set_lid`
  - **Params**: `{"on": true}` 或 `{"on": false}`
  - **ACK Result**: `{"lid_on": true/false}`
* **快捷控制方式**:
  - **Action**: `lid_on` 或 `lid_off`
  - **Params**: 无

---

### 5.3 S3 运行模型管理 (S3 Model Management)

服务器可通过以下指令查询和控制 S3 接收端芯片上存储与加载的本地 tflite 机器学习模型。

#### 5.3.1 查看当前运行模型详情 (Query Current Model Info)
* **Action**: `model_info`
* **Params**: 无
* **网关执行流**:
  1. 网关收到 `model_info` 后，向 S3 发送串口指令。
  2. S3 返回后，网关向服务器发送异步提交请求：
     - **URL**: `POST http://*.*.*.*/api/p4/submit?key=***` <!-- Censored Server API / 已脱敏服务器API -->
     - **JSON Payload**:
       ```json
       {
         "type": "model_info",
         "did": "p4-01",
         "data": {
           "name": "fruit_model_v1",
           "version": "1.0.0",
           "classes": ["apple", "banana", "orange"],
           "size": 53120,
           "accuracy": 0.95
         }
       }
       ```

#### 5.3.2 查看加载的所有模型列表 (Query Model List)
* **Action**: `model_list`
* **Params**: 无
* **网关执行流**:
  1. 网关向 S3 查询所有已烧录的模型文件。
  2. 网关向服务器异步提交列表数据：
     - **URL**: `POST http://*.*.*.*/api/p4/submit?key=***` <!-- Censored Server API / 已脱敏服务器API -->
     - **JSON Payload**:
       ```json
       {
         "type": "model_list",
         "did": "p4-01",
         "data": {
           "active_index": 0,
           "models": [
             {
               "name": "fruit_model_v1",
               "size": 53120,
               "active": true,
               "classes": ["apple", "banana", "orange"]
             },
             {
               "name": "rotten_detect",
               "size": 48200,
               "active": false,
               "classes": ["fresh", "rotten"]
             }
           ]
         }
       }
       ```

#### 5.3.3 切换当前运行模型 (Switch Model)
* **Action**: `model_switch`
* **Params**: `{"model_index": 1}` (目标模型在列表中的索引)
* **ACK Result**: 无

#### 5.3.4 删除指定模型 (Delete Model)
* **Action**: `model_delete`
* **Params**: `{"model_index": 1}`
* **ACK Result**: 无

---

### 5.4 远程维护辅助指令 (Remote Maintenance Commands)
* **网关重启 (Reboot)**:
  - **Action**: `reboot`
  - **Params**: 无
* **采集一次传感器数据并更新 UI (Trigger Collection)**:
  - **Action**: `collect`
  - **Params**: 无
* **广播警告通知 (Broadcast Alert)**:
  - **Action**: `alert`
  - **Params**: `{"msg": "Temperature is too high!"}`

