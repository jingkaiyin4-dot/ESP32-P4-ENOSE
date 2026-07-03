# 🛸 P4 AI 自动控制语音播报技术对接文档 (V3.2)

---

## 一、概述 (Overview)

当 AI 自动控制触发时，P4 接收到 `ai_control` 指令后，除了执行本地外设控制（风扇/灯光/舱盖）以外，还需要将 `reason` 文本通过 WebSocket 发送给云端语音服务，用以获取并实时播放 TTS 语音。

整个流程遵循**按需连接、播完即断**的低功耗、低资源占用设计原则，不占用网关侧常驻的系统资源。

---

## 二、触发来源 (Trigger Sources)

P4 接收决策文本 `reason` 共有以下两种途径（双通道触发）：

### 途径 A: 传感器数据上报响应 (Telemetry Response)
网关向 `POST /sensor/data` 发送遥测数据后，服务器在 HTTP 响应体中直接下发决策数据：
```json
{
  "ai_control": {
    "uv_on": true,
    "fog_on": false,
    "fan_on": true,
    "lid_on": true,
    "take_photo": true,
    "reason": "检测到CO2超标和气味异常，已开启风扇通风换气并启动紫外杀菌"
  }
}
```

### 途径 B: 异步控制轮询 (Command Polling)
网关定期向 `GET /p4/poll` 轮询指令队列时，服务器返回控制帧：
```json
{
  "id": "cmd_1782914450958",
  "action": "ai_control",
  "params": {
    "fan": 1,
    "lid": 1,
    "fog": 0,
    "uv": 1,
    "photo": 1,
    "reason": "检测到CO2超标和气味异常，已开启风扇通风换气并启动紫外杀菌"
  }
}
```

> 💡 **业务处理规则**：P4 统一提取 `params.reason` 作为 TTS 的播报文本。如果 `reason` 为空或不存在，则跳过本次语音播报流。

---

## 三、语音播报流程 (Voice Flow)

网关与自建多模态服务器握手并播放 TTS 的时序流程如下：

1. **获取文本**：P4 网关收到并校验 `reason` 文本。
2. **建立连接**：拉起并建立与 `ws://*.*.*.*/p4/ws` 的 WebSocket 握手。 <!-- Censored Server API / 已脱敏服务器API -->
3. **确认会话**：等待服务端下发 `hello` 握手应答，提取 `session_id`。
4. **发起合成**：网关向服务器推送 TTS 文本播放请求。
5. **解码播放**：接收服务端下发的二进制音频包，通过本地 Opus 解码器实时还原为 PCM 信号驱动扬声器发声。
6. **回收销毁**：收到服务端下发的文本结束状态 `stop` 信号后，延时 1.5s（确保硬件缓冲区播完），断开 WebSocket 连接并销毁后台所有编解码任务。

### 客户端时序伪代码 (Pseudocode)
```python
# 1. 建立长连接
ws = websocket_connect("ws://*.*.*.*/p4/ws") # Censored Server API / 已脱敏服务器API

# 2. 接收握手并获取 Session ID
hello = ws.recv_json()
session_id = hello["session_id"]

# 3. 发送 TTS 文本请求
tts_request = {
  "session_id": session_id,
  "type": "tts",
  "text": reason
}
ws.send(json.dumps(tts_request))

# 4. 实时流处理循环
opus_decoder = opus_create_decoder(16000, 1)
while True:
    frame = ws.recv()
    if is_text(frame):
        msg = json.loads(frame)
        if msg["type"] == "tts" and msg["state"] == "start":
            # 初始化音频硬件
            audio_output_init(16000)
        elif msg["type"] == "tts" and msg["state"] == "stop":
            # 停止解码并断开连接
            audio_output_close()
            ws.close()
            break
    elif is_binary(frame):
        # 解析 V2 协议头 (16字节) 并解码载荷
        header = frame[0:16]
        opus_data = frame[16:]
        pcm = opus_decode(opus_decoder, opus_data)
        audio_play(pcm)
```

---

## 四、WebSocket 协议细节 (Protocol Details)

### 4.1 服务端 hello 握手响应
```json
{
  "type": "hello",
  "transport": "websocket",
  "session_id": "session_multimodal_fc37d41e-596",
  "audio_params": {
    "sample_rate": 16000
  }
}
```

### 4.2 P4 TTS 请求报文
```json
{
  "session_id": "session_multimodal_fc37d41e-596",
  "type": "tts",
  "text": "检测到CO2超标和气味异常，已开启风扇通风换气并启动紫外杀菌"
}
```

### 4.3 服务端播放流控通知 (流状态包)
* **播放启动 (Start)**：
  ```json
  { "type": "tts", "state": "start", "sample_rate": 16000 }
  ```
* **播放结束 (Stop)**：
  ```json
  { "type": "tts", "state": "stop" }
  ```

### 4.4 二进制音频帧结构 (V2 协议头封装)
每个流式发送的二进制 BINARY 帧（Op-code `0x02`）均由 **16 字节大端协议头** 与 **Opus 原始数据载荷** 组成：

| 字节偏移 (Bytes) | 字段名称 (Name) | 类型 (Type) | 规范约束 (Specification) |
| :--- | :--- | :--- | :--- |
| `[0..1]` | version | `uint16` | 固定填入 `2`（大端序：`0x0002`） |
| `[2..3]` | type | `uint16` | 载荷类型，固定填入 `0`（Audio，大端序：`0x0000`） |
| `[4..7]` | reserved | `uint32` | 保留字段，固定填入 `0`（`0x00000000`） |
| `[8..11]` | timestamp | `uint32` | 播放时序相对时间戳，单位毫秒 (大端序) |
| `[12..15]` | payload_len | `uint32` | 紧随其后的 Opus 载荷长度 (大端序) |
| `[16..]` | opus_data | `bytes` | Opus 原始音频数据包 |

> 🎙️ **音频输出流物理标准**：Opus 编码、16000 Hz 采样率、单声道 (Mono)、20ms 帧长。

---

## 五、P4 网关端实现要点 (Client Implementation)

1. **Opus 解码器参数配置**：固定使用 16000Hz、单声道（1 Channel）初始化。
2. **实时零拷贝直驱**：每帧解码出的 PCM 缓存直接送入 I2S/ES8311 环形缓冲区播放。
3. **播完延时关断**：收到 `"state":"stop"` 流控通知后，必须主动延时 1.5 秒再执行 `xiaozhi_ai_service_stop()`，确保音频 DMA 队列完全播空，防范截断噪音。
4. **握手与数据超时机制**：如连接建立后 10 秒内未收到服务端的 `hello` 应答或数据帧，客户端应视为链路死锁，强行断开并执行重连退避。
5. **网络容错**：连接若由于网络限制或 DNS 解析失败而无法接通，客户端执行静默放弃并记录 Warn 日志，绝对不阻塞系统的传感器遥测主任务循环。
6. **资源复用**：系统中的 Opus 解码器在生命周期内复用，避免在短时间内因多次决策触发反复申请/释放堆内存造成系统碎片化。
7. **首尾过滤**：去除所有的换行及空格符，为空的 `reason` 不发起 WebSocket 会话。

---

## 六、音频硬件配置参数 (ES8311)

* **采样率**：16000 Hz
* **位宽**：16 bit (16-bit Signed PCM)
* **声道数**：单声道
* **I2S 工作模式**：Master TX
* **端侧环形缓冲区大小**：不小于 8192 字节

---

## 七、测试与验证方法

1. **后台下发命令**：在系统网页端/管理后台点击“测试自动控制”按钮，向队列中写入一则 `ai_control` 指令。
2. **网关拉起流程**：观察 P4 串口日志，确认指令被成功获取。检查是否相继执行了继电器物理闭合操作，并且 `reason` 成功解析展示在弹窗中。
3. **语音流日志监视**：
   * 确认控制台输出 `Starting XiaoZhi service for temporary TTS broadcast`。
   * 确认握手成功并下发：`Sent TTS text to server`。
   * 音频流开始流入：扬声器流畅发出对应语音。
