# ESP32-P4 电子鼻系统 — 功能报告

## 1. 项目概述

本项目是一个基于 **ESP32-P4** 功能开发板的 **电子鼻（E-Nose）边缘感知系统**，集成了气体传感器数据采集、AI 气体分析、语音交互、摄像头扫码、Web 仪表盘等功能。系统采用双芯片架构：ESP32-P4 负责主控和 UI 渲染，ESP32-C6 作为协处理器提供 Wi-Fi 和 BLE 连接（通过 ESP-Hosted SDIO 桥接）。

**硬件平台**：ESP32-P4 Function EV Board（SC2336 MIPI CSI 摄像头 + 7 寸 MIPI DSI 显示屏 + ES8311 Codec + BME680 传感器）

**目标应用场景**：食品新鲜度检测、环境气体监测、智慧农业果蔬成熟度判断等。

---

## 2. 系统架构

```
┌──────────────────────────────────────────────────────┐
│                    ESP32-P4 (主控)                      │
│  ┌─────────┐ ┌──────────┐ ┌───────────┐ ┌──────────┐  │
│  │ LVGL UI │ │ XiaoZhi  │ │  Camera   │ │   Web    │  │
│  │ (3 屏幕) │ │ AI Voice │ │ QR Scanner│ │Dashboard │  │
│  └────┬────┘ └────┬─────┘ └─────┬─────┘ └────┬─────┘  │
│       │           │             │             │        │
│  ┌────┴───────────┴─────────────┴─────────────┴─────┐  │
│  │              主应用层 (ui.cpp / main.cpp)          │  │
│  └─────────────────────┬────────────────────────────┘  │
│                        │                               │
│  ┌─────────┐ ┌────────┴──────┐ ┌──────────────────┐  │
│  │ BME680  │ │E-Nose Gateway │ │  BLE Central     │  │
│  │ (I2C)  │ │(Volcano AI)   │ │  (via C6)        │  │
│  └─────────┘ └───────────────┘ └────────┬─────────┘  │
│                                           │            │
│  ┌────────────────────────────────────────┴──────────┐│
│  │         ESP-Hosted (SDIO) → ESP32-C6 协处理器       ││
│  │         Wi-Fi STA │ BLE NimBLE                     ││
│  └───────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────┘
          │ SDIO                        │ BLE
┌─────────┴──────┐           ┌──────────┴──────────┐
│  ESP32-C6      │           │  ESP32-S3 气体传感器  │
│  Wi-Fi + BLE   │           │  VOC/CO2/Ethylene    │
│  协处理器       │           │  (S3-A, S3-B 节点)    │
└────────────────┘           └─────────────────────┘
```

---

## 3. 功能模块详解

### 3.1 LVGL 用户界面系统

**实现文件**：`main/ui.cpp`（~1893 行）、`main/ui.h`

**UI 框架**：LVGL v9，通过 `esp_lvgl_adapter` 桥接到 MIPI DSI 显示屏，分辨率 1024×600。

**屏幕架构（3 屏）**：

#### 3.1.1 启动屏幕（`scr_startup`）
- 项目标题 "E-Nose System"
- Wi-Fi 连接按钮（点击后扫描 AP 列表，输入密码连接）
- "Enter Dashboard" 入口按钮
- Wi-Fi 连接模态框（扫描列表 + 密码输入 + 虚拟键盘）

#### 3.1.2 主仪表盘屏幕（`scr_main`）
- **标题栏**：项目名 + 副标题
- **环境传感器面板**：BME680 温度/湿度/气压实时显示
- **S3-A 节点面板**：VOC/CO2/乙烯浓度（BLE 数据源）
- **S3-B 节点面板**：VOC/CO2/乙烯浓度（BLE 数据源）
- **AI 交互中心**：
  - 状态图标 + 状态文字 + 副标题
  - PTT（Push-To-Talk）按钮：按下开始语音识别，松开触发 AI 思考
  - 状态实时反映 XiaoZhi 服务状态（Ready/Listening/Thinking/Speaking/Error）
- **音量控制面板**：滑块 + 百分比显示
- **QR 扫描入口按钮**：进入摄像头扫码屏幕
- **AI 分析按钮**（条件编译 `CONFIG_ELEC_NOSE_GATEWAY_ENABLE`）：触发火山引擎 AI 分析
- **控制中心**（下拉手势触发）：
  - Wi-Fi 状态磁贴（点击可重新扫描）
  - 亮度滑块
  - 音量滑块
  - 关闭提示

#### 3.1.3 QR 扫描屏幕（`scr_qr`）
- 摄像头实时画面（LVGL `lv_img` 对象 + `lv_draw_buf_t`）
- QR 扫描定位框（200×200 绿色边框）
- 扫描提示文字
- QR 解码结果面板（节点 ID + 传感器数据）
- 返回按钮（停止摄像头并释放资源）

**实现细节**：
- 全局禁用阴影样式（`g_style_no_shadow`），减少 LVGL 渲染负载
- CJK 字体回退：Montserrat 14 + 思源黑体 14
- 数据刷新使用 `lv_timer`（500ms 间隔），从 mutex 保护的共享结构读取传感器数据
- 摄像头帧回调中获取 LVGL 互斥锁（`esp_lv_adapter_lock`），通过 `lv_draw_buf_init` + `lv_image_set_src` 每帧更新画面

---

### 3.2 BLE 传感器数据接收

**实现文件**：`main/ble_central.cpp`（~421 行）、`main/ble_central.h`

**功能**：P4 通过 ESP-Hosted 桥接使用 C6 的 BLE 控制器，运行 NimBLE 协议栈作为 Central 角色接收 S3 传感器节点的数据。

**工作流程**：
1. `ble_central_init()` 初始化 NimBLE，注册 GAP 同步回调
2. BLE 同步后开始扫描，匹配自定义 128-bit UUID (`aa ca a7 d7 fe 6f 1a 8a 4b 0c 7a 0a eb e0 cc b0`) 或设备名包含 "S3"/"E-Nose" 的广播
3. 发现匹配设备后停止扫描并发起连接
4. 连接建立后进行 MTU 协商，然后发现所有服务和特征
5. 找到支持 Notify 的特征后，写入 CCCD 订阅通知
6. 收到 Notify 数据后解析 JSON 格式的传感器数据：`{"node":"S3_A","voc":0.0,"co2":0,"eth":0.0}`
7. 通过回调函数 `s_user_cb` 将数据传递给 UI 层

**节点管理**：
- 最多支持 4 个 BLE 外设同时连接（`PEER_MAX = 4`）
- 断线后自动重新扫描连接
- 支持按节点名 "S3_A"/"S3_B" 分别存储数据

**JSON 解析**：使用 cJSON 库，提取 `node`/`voc`/`co2`/`eth` 四个字段。

---

### 3.3 XiaoZhi AI 语音助手

**实现文件**：`main/xiaozhi_ai_service.c`（~877 行）、`main/xiaozhi_ai_service.h`

**功能**：集成小智（XiaoZhi）AI 语音助手，实现全双工语音对话。

**协议**：XiaoZhi V2 二进制协议（WebSocket over TLS）
- 帧头 16 字节：`version(2) + type(0=Audio/1=JSON) + reserved + timestamp + payload_size`
- 语音编解码：Opus（16kHz mono, 20ms 帧长, 12kbps）

**工作流程**：
1. **初始化** `xiaozhi_ai_service_init()`：
   - 创建 PCM/Opus 双 RingBuffer（PSRAM 分配）
   - 初始化 ES8311 Codec（16kHz/16bit/Stereo）
   - 创建 Opus 编解码器
   - 启动 3 个音频任务：`mic_task`、`decode_task`、`speaker_task`

2. **激活** `xiaozhi_ai_service_start()`：
   - 启用 GPIO 53 PA 功放
   - 启动 `xiaozhi_main_task`
   - 通过 HTTPS POST `api.tenclass.net/xiaozhi/ota/` 进行设备激活
   - 未激活时在串口输出激活码，用户需访问 xiaozhi.me 输入

3. **WebSocket 连接**：
   - 获取 Token 和 WS URI 后建立 WSS 连接
   - 发送 V2 Hello 消息，协商 Opus 音频格式
   - 收到 Server Hello 后进入 CONNECTED 状态

4. **PTT 语音交互**：
   - 按下 PTT：发送 `{"type":"listen","state":"start","mode":"manual"}` 开始录音
   - `mic_task` 从 I2S 读取立体声 → 降混为单声道 → Opus 编码 → V2 帧头 + 载荷 → WebSocket 二进制发送
   - 松开 PTT：发送 `{"type":"listen","state":"stop"}` 停止录音并触发 AI 思考
   - AI 回复时：`decode_task` 从 Opus RingBuffer 取帧 → V2 帧头解析 → Opus 解码 → PCM RingBuffer
   - `speaker_task` 从 PCM RingBuffer 取数据 → 单声道转立体声 → I2S 写入 ES8311 播放

5. **状态机**：IDLE → CONNECTING → CONNECTED → LISTENING → THINKING → SPEAKING → 回到 CONNECTED

**资源分配**：
- Opus RingBuffer: 64KB (PSRAM)
- PCM RingBuffer: 128KB (PSRAM)
- Mic V2 发送缓冲: 静态 PSRAM 分配
- Opus 解码缓冲: 5760 bytes (2880 samples × 2)

---

### 3.4 摄像头与 QR 码扫描

**实现文件**：`main/app_video.c`（~500 行）、`main/app_video.h`、`ui.cpp` 中的 `camera_frame_cb`

**硬件**：SC2336 MIPI CSI 摄像头传感器，1280×720 @ 30fps，RGB565 输出

**视频管线**：
1. `app_video_main()`：通过 I2C SCCB 初始化摄像头传感器
2. `app_video_open()`：打开 `/dev/video0`，协商 RGB565 格式，设置 30fps
3. `app_video_set_bufs()`：分配 4 个 PSRAM 对齐的帧缓冲（USERPTR 模式）
4. `app_video_stream_task_start()`：启动 V4L2 流采集任务
5. 帧回调 `camera_frame_cb()`：每帧处理逻辑如下

**帧回调处理流水线**：
1. **JPEG 预编码**：缩放到 256×144 + RGB565→RGB888 + JPEG 编码（q=50），供 Web 端使用
2. **帧率控制**：每 4 帧取 1 帧用于显示（`frame_skip_counter % 4`）
3. **QR 码识别**：每 4 个显示帧取 1 个做 QR 解码
   - 缩放到 200×150 灰度图
   - 使用 quirc 库解码
   - 识别 "S3_A"/"S3_B" 等节点标识
   - 在屏幕上显示解码结果
4. **LVGL 显示**：
   - 双缓冲显示（2 个 PSRAM 缓冲交替）
   - 获取 LVGL 互斥锁
   - `lv_draw_buf_init()` 初始化 draw buffer
   - `lv_image_set_src()` 设置图像源
   - 首帧居中定位，后续帧自动刷新

**资源管理**：
- 摄像头缓冲: 4 × PSRAM（USERPTR 模式）
- 显示缓冲: 2 × PSRAM
- QR 灰度缓冲: 200×150 = 30KB PSRAM
- quirc 解码器: 动态创建/销毁
- 返回时完整释放所有资源

---

### 3.5 Web 仪表盘

**实现文件**：`main/web_dashboard.cpp`（~409 行）、`main/web_dashboard.h`

**功能**：内置 HTTP 服务器，提供 Web 端传感器数据可视化和摄像头实时画面。

**HTTP 端点**：

| 路径 | 方法 | 功能 |
|------|------|------|
| `/` | GET | HTML 仪表盘页面 |
| `/api/data` | GET | JSON 格式传感器数据 |
| `/camera.jpg` | GET | JPEG 摄像头帧 |

**前端页面**（内嵌 HTML/JS）：
- 响应式 Grid 布局，深色主题
- 环境传感器卡片（温度/湿度/气压）
- Node S3-A/S3-B 传感器卡片（VOC/CO2/乙烯 + 在线状态指示灯）
- AI 分析结果区域（支持中文长文本换行显示）
- 摄像头实时画面（preload-then-swap 无闪烁模式，~5fps）
- 数据每 3 秒自动刷新

**JPEG 预编码引擎**：
- 在摄像头帧回调中预编码（视频任务上下文），HTTP 处理程序只做内存拷贝
- 双缓冲 + 自旋锁实现零等待无锁交接
- 缩放：1280×720 → 256×144（固定点步进，无逐像素除法）
- 色彩转换：RGB565 → RGB888（5-6-5 到 8-8-8 精确扩展）
- JPEG 编码：ESP-IDF `esp_jpeg_enc`，q=50，420 子采样
- 每 3 帧编码 1 帧（降低 CPU 负载）

**JSON API 响应格式**：
```json
{
  "environment": {"temperature": 25.0, "humidity": 50.0, "pressure": 1013.0},
  "node1": {"valid": true, "voc": 1.2, "co2": 520, "eth": 2.3},
  "node2": {"valid": true, "voc": 2.5, "co2": 680, "eth": 1.8},
  "ai_result": "...",
  "camera": {"available": true, "width": 256, "height": 144}
}
```

---

### 3.6 电子鼻 AI 网关

**实现文件**：`components/electronic_nose_gateway/electronic_nose_gateway.c`（~420 行）、`include/electronic_nose_gateway.h`

**功能**：接收传感器数据，调用火山引擎豆包大模型（doubao-seed-1.6）进行 AI 气体分析。

**工作流程**：
1. `electronic_nose_gateway_init()`：注册 Wi-Fi 事件处理器
2. `electronic_nose_gateway_trigger_analysis(sensor_data, len)`：手动触发分析
   - 接收 JSON 格式的传感器数据（包含 voc/co2/eth/hcho/co/temp/humi）
   - 构造系统提示词（电子鼻分析专家角色 + 传感器类型说明 + 输出格式要求）
   - 通过 HTTPS POST 调用 `*.*.*.*/v1/chat/completions` <!-- Censored / 已脱敏 -->
   - 使用 Bearer Token 认证
   - 解析 AI 响应中的 `content` 字段
   - 通过回调 `gateway_result_cb_t` 返回分析结果给 UI 层

**Kconfig 配置**：
- `CONFIG_ELEC_NOSE_GATEWAY_ENABLE`：启用/禁用
- `CONFIG_ELEC_NOSE_UDP_PORT`：UDP 监听端口（默认 8888）
- `CONFIG_ELEC_NOSE_API_KEY`：火山引擎 API Key
- `CONFIG_ELEC_NOSE_MODEL_URL`：模型 API URL

**AI 分析结果展示**：
- UI 层弹出全屏对话框（940×540）
- 使用 CJK 字体回退显示中文分析结果
- 支持长文本自动换行

---

### 3.7 BME680 环境传感器

**实现文件**：`components/bme680_sensor/bme680.c`（~182 行）、`bme680.h`

**功能**：通过 I2C 读取 BME680 传感器的温度、湿度、气压数据。

**API**：
- `bme680_init(i2c_bus)`：初始化，校验芯片 ID（0x61），配置过采样率和燃气加热器
- `bme680_read(&temp, &hum, &press)`：触发 Forced 模式测量，等待 100ms 后读取
- `bme680_deinit()`：释放 I2C 设备

**实现特点**：
- 使用 ESP-IDF 新版 `i2c_master` 驱动 API
- I2C 地址 0x77，速率 400KHz
- 温度/湿度/气压通过 ADC 值简化公式计算（未使用 Bosch 官方补偿算法）
- 全局变量 `g_temperature`/`g_humidity`/`g_pressure` 供 UI 和 Web 端读取

> 注：当前 BME680 采集任务已从 `main.cpp` 中移除（注释说明为避免 I2C 总线与触摸/音频冲突），数据保持默认值。

---

### 3.8 Wi-Fi 连接管理

**实现文件**：`main/ui.cpp` 中的 Wi-Fi 相关函数

**功能**：完整的 Wi-Fi STA 模式管理，包含扫描、连接、静态 IP 配置。

**实现**：
- 静态 IP 配置：`192.168.110.100` / 网关 `192.168.110.1` / DNS `114.114.114.114`
- 延迟初始化：首次点击 Wi-Fi 按钮时才初始化 Wi-Fi 栈
- 扫描流程：
  1. 断开当前连接
  2. 调用 `esp_wifi_scan_start()` 扫描
  3. 最多发现 60 个 AP
  4. 结果通过 LVGL 列表展示
  5. 选中 AP 后弹出密码输入框 + 虚拟键盘
  6. 连接后状态定时器轮询（500ms），成功后显示绿色 SSID
- 断线自动重连
- DNS 保障：连接成功后检查 DNS 设置，若为空则强制写入

---

### 3.9 ESP-Hosted 双芯片通信

**配置文件**：`sdkconfig.defaults`

**功能**：ESP32-P4 通过 SDIO 接口与 ESP32-C6 协处理器通信，获取 Wi-Fi 和 BLE 能力。

**关键配置**：
- BLE over ESP-Hosted：`CONFIG_BT_NIMBLE_ENABLED=y` + `CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=y`
- NimBLE HCI 通过 VHCI 传输
- SDIO 队列优化：TX/RX 队列 256，时钟 20MHz
- Wi-Fi BA 窗口优化：TX/RX BA Win = 32
- TCP 优化：发送缓冲 65534，TCP SACK 启用
- lwIP socket 数量：20

---

## 4. 音频系统

**硬件**：ES8311 Codec + I2S 接口 + GPIO 53 PA 功放

**BSP 扩展**：`components/bsp_extra/`

**API**：
- `bsp_extra_codec_init()`：初始化 Codec 录放句柄
- `bsp_extra_i2s_read/write()`：I2S 读写音频数据
- `bsp_extra_codec_volume_set()`/`bsp_extra_codec_mute_set()`：音量/静音控制
- `bsp_extra_codec_set_fs()`：动态切换采样率（16kHz ↔ 24kHz）

**音频参数**：16kHz/16bit/Stereo（I2S 兼容），实际麦克风使用单声道降混

---

## 5. 系统资源配置

### 5.1 Flash 分区

| 分区 | 类型 | 大小 | 说明 |
|------|------|------|------|
| nvs | data/nvs | 24KB | 非易失性存储 |
| phy_init | data/phy | 4KB | PHY 校准数据 |
| factory | app/factory | 9MB | 应用程序 |
| storage | data/spiffs | 4MB | 音频文件存储 |

### 5.2 PSRAM 使用

- LVGL 自定义内存分配器（`LV_MEM_CUSTOM` → `heap_caps_malloc(SPIRAM)`）
- 摄像头缓冲：4 × 帧大小（USERPTR 模式）
- 显示缓冲：2 × 帧大小
- Opus/PCM RingBuffer：64KB + 128KB
- JPEG 编码缓冲：2 × 48KB
- XiaoZhi Mic V2 发送缓冲
- Opus 解码缓冲：5760 bytes
- 视频流任务栈：32KB

### 5.3 FreeRTOS 任务

| 任务名 | 栈大小 | 优先级 | 核心 | 说明 |
|--------|--------|--------|------|------|
| lvgl | 系统分配 | 高 | 1 | LVGL 渲染主循环 |
| video stream task | 32KB | 3 | 0 | 摄像头帧采集 |
| mic_task | 20KB | 5 | 1 | 麦克风音频采集 + Opus 编码 |
| decode_task | 8KB | 5 | 1 | Opus 解码 |
| speaker_task | 8KB | 6 | 1 | 音频播放 |
| xiaozhi_main | 8KB | 5 | 1 | XiaoZhi WebSocket 主任务 |
| wifi_scan | 12KB | 5 | 1 | Wi-Fi AP 扫描 |
| ble_host_task | NimBLE 管理 | — | — | BLE 协议栈 |

### 5.4 LVGL 性能优化

- 渲染刷新周期：30ms（~33fps）
- 层缓冲：204800 bytes
- 图像缓存：20 条目
- 渐变缓存：10240 bytes
- 禁用复杂软件渲染（`LV_DRAW_SW_COMPLEX=n`）
- Task Watchdog 超时：10 秒
- 不检查 CPU0 IDLE 任务（防止渲染阻塞误触发）

---

## 6. 数据流总览

```
ESP32-S3 传感器节点 (VOC/CO2/Ethylene)
    │
    ├── BLE 广播 ──→ ESP32-C6 ──SDIO──→ ESP32-P4 BLE Central
    │                                         │
    │                                    ui_handle_sensor_data()
    │                                         │
    │                                   ┌─────┴──────┐
    │                                   │ UI Label 更新  │
    │                                   │ Web API 数据   │
    │                                   │ AI 分析输入    │
    │                                   └────────────┘
    │
SC2336 摄像头
    │
    ├── MIPI CSI ──→ V4L2 帧采集
                        │
                  camera_frame_cb()
                        │
            ┌───────────┼───────────┐
            │           │           │
     LVGL 显示     QR 解码    JPEG 编码
            │           │           │
       屏幕画面    节点识别    Web /camera.jpg
```

---

## 7. 构建与配置

### 7.1 编译依赖

| 依赖项 | 用途 |
|--------|------|
| ESP-IDF v5.5.2 | 基础框架 |
| LVGL v9 | UI 渲染 |
| esp_lvgl_adapter | LVGL ↔ MIPI DSI 桥接 |
| NimBLE | BLE 协议栈 |
| ESP-Hosted | P4↔C6 SDIO 通信 |
| esp_websocket_client | XiaoZhi WSS 连接 |
| Opus | 音频编解码 |
| cJSON | JSON 解析 |
| quirc | QR 码解码 |
| esp_jpeg_enc | JPEG 硬件编码 |
| mbedtls (TLS) | HTTPS/WSS 安全连接 |

### 7.2 关键 Kconfig 选项

```
CONFIG_ELEC_NOSE_GATEWAY_ENABLE=y     # 电子鼻 AI 网关
CONFIG_XIAOZHI_AI_ENABLED=1            # 小智语音助手
CONFIG_CAMERA_SC2336=y                 # SC2336 摄像头
CONFIG_BT_NIMBLE_ENABLED=y             # BLE 支持
CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=y  # BLE over ESP-Hosted
CONFIG_SPIRAM=y                        # PSRAM 启用
CONFIG_LV_MEM_CUSTOM=y                 # LVGL 使用 PSRAM
CONFIG_COMPILER_OPTIMIZATION_SIZE=y   # 体积优化
```

---

## 8. 已知问题与注意事项

1. **BME680 冲突**：I2C 总线与触摸屏/音频共用，为避免总线争用，BME680 采集任务当前已禁用
2. **摄像头显示**：不使用 `LV_IMAGE_FLAGS_ALLOCATED`，否则会导致 `draw_buf_flush` 中渲染管线卡死（看门狗超时）
3. **IRAM 紧张**：ESP32-P4 IRAM 有限，已关闭 `LWIP_IRAM_OPTIMIZATION` 和 `LV_ATTRIBUTE_FAST_MEM_USE_IRAM`
4. **Opus 栈溢出**：增大 pseudostack 至 180000 并优先使用 PSRAM
5. **BLE 传感器数据**：当前通过 BLE Notify 接收，UDP 监听方式已弃用但代码保留（`udp_listener_task` 被注释）
6. **摄像头初始化**：`app_video_main()` 只能调用一次，重复调用会因 ISP 设备已注册而失败

---

## 9. 文件清单

| 文件 | 行数 | 说明 |
|------|------|------|
| `main/main.cpp` | 329 | 应用入口，初始化各子系统 |
| `main/ui.cpp` | 1893 | LVGL 界面（3 屏幕）+ 传感器数据管理 + Wi-Fi |
| `main/ui.h` | 23 | UI 公共接口 |
| `main/ble_central.cpp` | 421 | BLE Central 传感器数据接收 |
| `main/xiaozhi_ai_service.c` | 877 | XiaoZhi 语音助手 |
| `main/web_dashboard.cpp` | 409 | HTTP 服务器 + JPEG 流 |
| `main/app_video.c` | 500 | V4L2 摄像头驱动 |
| `main/lvgl_adapter_init.c` | — | LVGL 适配器初始化 |
| `sdkconfig.defaults` | 149 | 项目默认配置 |
| `partitions.csv` | 5 | Flash 分区表 |

---

*文档生成日期：2026-05-12*
