# 🛰️ ESP32-P4 网关 与 云端服务器通信协议规格书 (联调生产版)

> **版本**：v2.1 (联调生产版) | **基准端口**：`80` (通过 Nginx `/api` 路由反代)
> **全局认证 Key**：`bigboss` (URL参数 `?key=bigboss` 或请求头 `X-API-Key: bigboss`)

---

## 1. 接口总览

网关采用**扁平、紧凑的 JSON 数据结构**以节省嵌入式设备的 PSRAM 与网络带宽。

| 方向 | 终点路由 (Endpoint) | 方法 | 频率/触发时机 | 用途说明 |
| :--- | :--- | :--- | :--- | :--- |
| **P4 → 云端** | `/api/sensor/data?key=bigboss` | `POST` | **1 Hz 高频** | 传感器实时读数上报 |
| **P4 → 云端** | `/api/p4/submit?key=bigboss` | `POST` | 异步事件触发 | 主动状态上报（设备模型信息、模型列表） |
| **P4 → 云端** | `/api/p4/ack?key=bigboss` | `POST` | 收到命令后立即触发 | 命令执行确认与结果回复 |
| **云端 → P4** | `/api/p4/poll?key=bigboss` | `GET` | 5秒轮询（快上报时1秒） | 网关主动拉取待执行的控制命令 |

---

## 2. 详细接口规格

### 2.1 传感器数据上报 (1 Hz 高频)
网关实时采集下位机传感器数据后，将数据包装为紧凑的扁平 JSON 格式，向云端进行 `HTTP POST`。

* **请求 URL**：`http://*.*.*.*/api/sensor/data?key=***` <!-- Censored Server API / 已脱敏服务器API -->
* **Content-Type**：`application/json`
* **网关发送的请求体 (JSON)**：
```json
{
  "t": 24.4,
  "hu": 58.2,
  "co2": 669,
  "o": 6.0,
  "h": 0.1,
  "c": 17.2,
  "v": 20.6,
  "cls": "banana_fresh",
  "conf": 1.00,
  "fr": 50
}
```

* **核心字段映射表 (供服务端解析并存储入库)**：
| 网关字段 | 服务端数据库对应键名 | 数据类型 | 含义 | 单位/范围 | 示例值 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`t`** | `temp` | Float | 温度 | ℃ | 24.4 |
| **`hu`** | `hum` | Float | 湿度 | %RH | 58.2 |
| **`co2`** | `co2` | Integer | 二氧化碳浓度 | ppm | 669 |
| **`o`** | `odor` | Float | 气味传感器原始值 | raw | 6.0 |
| **`h`** | `hcho` | Float | 甲醛传感器原始值 | raw | 0.1 |
| **`c`** | `co` | Float | 一氧化碳传感器原始值 | raw | 17.2 |
| **`v`** | `voc` | Float | TVOC 传感器原始值 | raw | 20.6 |
| **`cls`** | `cls` | String | AI 推理分类预测标签 | string | `"banana_fresh"` |
| **`conf`** | `conf` | Float | AI 预测置信度 | 0.00 ~ 1.00 | 1.00 |
| **`fr`** | `fr` | Integer | 新鲜度评分 | 0 ~ 100 | 50 |

* **期望的服务端响应 (200 OK)**：
```json
{
  "status": "ok"
}
```

---

### 2.2 主动状态与模型上报 (事件触发)
网关采用异步非阻塞方式，在检测到“下位机就绪并开机握手成功”或“云端下发查询”时，向云端提交当前网关运行的模型指标或模型列表。

* **请求 URL**：`http://*.*.*.*/api/p4/submit?key=***` <!-- Censored Server API / 已脱敏服务器API -->
* **Content-Type**：`application/json`

#### 场景 A：设备当前加载的模型信息上报 (`type = "model_info"`)
当网关热加载了新模型，或完成了下位机的模型参数同步时触发。
* **网关发送的请求体 (JSON)**：
```json
{
  "type": "model_info",
  "did": "p4-01",
  "data": {
    "name": "food_freshness",
    "version": "1.0.0",
    "classes": ["air", "banana_fresh", "apple_fresh", "orange_fresh"],
    "size": 4736,
    "accuracy": 0.95
  }
}
```
*(注：`classes` 为分类标签数组，`size` 为模型文件大小字节数)*

#### 场景 B：设备本地存储的模型列表上报 (`type = "model_list"`)
当云端请求查询网关本地 SD 卡的模型文件清单时触发。
* **网关发送的请求体 (JSON)**：
```json
{
  "type": "model_list",
  "did": "p4-01",
  "data": {
    "active_index": 0,
    "models": [
      {
        "name": "food_freshness",
        "size": 4736,
        "active": true,
        "classes": ["air", "banana_fresh", "apple_fresh", "orange_fresh"]
      },
      {
        "name": "meat_leakage",
        "size": 9812,
        "active": false,
        "classes": ["air", "meat_fresh", "meat_spoiled"]
      }
    ]
  }
}
```

---

### 2.3 命令确认上报 (收到命令并执行后触发)
网关在从云端拉取到控制命令后，**不论成功还是失败**，必须向云端回复确认报文（ACK），以便云端更新命令队列状态。

* **请求 URL**：`http://*.*.*.*/api/p4/ack?key=***` <!-- Censored Server API / 已脱敏服务器API -->
* **Content-Type**：`application/json`
* **网关发送的请求体 (JSON)**：
```json
{
  "id": "cmd_1780302494463",
  "accept": true,
  "result": {
    "uv_on": true
  },
  "reason": ""
}
```
* `id`：拉取命令时获取到的全局唯一 `id`。
* `accept`：网关是否成功执行该操作。`true` 代表成功，`false` 代表失败/拒绝。
* `result`：执行结果键值对（可选）。
* `reason`：拒绝或失败时的具体错误原因说明（仅在 `accept = false` 时填写）。

---

### 2.4 命令拉取轮询 (5 秒轮询)
网关定时向云端拉取是否有待执行的指令，如果没有指令，服务端应返回空队列标识；如果有，则返回具体指令。

* **请求 URL**：`http://*.*.*.*/api/p4/poll?key=***` <!-- Censored Server API / 已脱敏服务器API -->
* **Method**：`GET`
* **服务端无待执行命令时的响应 (200 OK)**：
```json
{
  "id": null
}
```
* **服务端有待执行命令时的响应 (200 OK) — 以控制 UV 灯为例**：
```json
{
  "id": "cmd_1780302494463",
  "action": "set_uv",
  "params": {
    "on": 1,
    "duration": 30
  }
}
```

* **支持的指令 Action 协议集 (服务端可下发的命令规范)**：
| action | params 参数 | 含义说明 |
| :--- | :--- | :--- |
| **`set_uv`** | `{"on": 0/1}` | 手动开关网关的紫外（UV）杀菌灯 |
| **`uv_auto_on` / `uv_auto_off`** | 无 | 切换 UV 灯自动触发机制开关 |
| **`uv_dur`** | `{"duration": 15}` | 设置自动紫外线持续开启时间（单位：分钟） |
| **`lid_on` / `lid_off`** | 无 | 控制物理实验盒舱门的舵机开/关 |
| **`alert`** | `{"msg": "检测到食物变质"}` | 触发 UI 弹窗警告 |
| **`reboot`** | 无 | 强制网关进行硬件重启 |
| **`model_info`** | 无 | 查询网关当前正加载的模型详细信息 |
| **`model_list`** | 无 | 查询网关本地 SD 卡已缓存的所有模型列表 |
| **`model_switch`** | `{"model_index": 1}` | 切换网关加载运行的模型（基于模型列表索引） |
| **`model_delete`** | `{"model_index": 1}` | 删除网关本地 SD 卡中缓存的某个模型 |
| **`update_model`** | `{"model_name": "food_freshness", "model_url": "http://...", "model_version": "1.1.0"}` | 触发网关进行 OTA 推送，从 URL 下载新的 TFLite 并热加载 |

---

## 3. 典型数据上报时序流程图 (网关与云端交互)

```
网关 (P4)                                               云端服务器 (Nginx / Web)
   │                                                         │
   │ 1. 高频数据上报 (1 Hz 周期触发)                           │
   ├─ POST /api/sensor/data?key=bigboss (Payload: 传感器JSON) ─→
   │                                                         │ (解析并写入 InfluxDB/MySQL)
   │                                                         ←─ 200 OK {status: "ok"}
   │                                                         │
   │                                                         │
   │ 2. 状态轮询 (5 秒周期触发)                                │
   ├─ GET /api/p4/poll?key=bigboss ──────────────────────────→
   │                                                         │ (查询是否有排队的用户控制指令)
   │                                                         ←─ 200 OK {id: "cmd_123", action: "set_uv", params: {on: 1}}
   │                                                         │
   │ 3. 执行硬件控制并立即回复确认                              │
   ├─ POST /api/p4/ack?key=bigboss (Payload: accept: true) ──→
   │                                                         │ (更新命令队列状态为已完成)
   │                                                         ←─ 200 OK {ok: true}
   │                                                         │
```
