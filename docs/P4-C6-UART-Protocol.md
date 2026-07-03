# P4-C6 UART 通信协议文档 (v2.2)

## 1. 物理层

| 参数 | P4 (主控) | C6 (桥接) |
|------|----------|-----------|
| UART | UART1 | Serial1 |
| TX | GPIO32 → C6 RX (GPIO17) | GPIO16 → P4 RX (GPIO36) |
| RX | GPIO36 ← C6 TX (GPIO16) | GPIO17 ← P4 TX (GPIO32) |
| 波特率 | 115200 | 115200 |
| 格式 | 8N1 | 8N1 |
| 消息分隔 | `\n` (换行) | `\n` (换行) |

所有消息均为 ASCII 文本，以 `\n` 结尾。P4 和 C6 均按行读取，忽略空行和回车。

---

## 2. P4 → C6 下行指令格式

### 2.1 `LISTEN:<设备名>`

```
LISTEN:S3_03\n
```

- 告诉 C6 监听一个设备（通过名称，不传 MAC）
- C6 自动从该设备后续发送的数据中学习 MAC
- C6 学到的 MAC 通过 `learned` 消息回复给 P4

**典型调用时机：** QR 扫码配对时、开机恢复配对列表时、心跳自愈重试时。

### 2.2 `CANCEL:<设备名>`

```
CANCEL:S3_03\n
```

- 取消监听指定设备

### 2.3 `CANCEL ALL`

```
CANCEL ALL\n
```

- 清空所有监听设备
- P4 每次发起新配对前会先发 `CANCEL ALL`

### 2.4 `CMD:<设备名>:<命令>

```
CMD:S3_03:uv_on\n
CMD:S3_03:fog_on\n
CMD:S3_03:model update food_freshness 48 4736 1.0.0\n
CMD:S3_03:model chunk 0 AABBCCDDEEFF001122...\n
CMD:S3_03:model list\n
```

- C6 将 `<命令>` 部分通过 ESP-NOW 原样转发给目标设备
- `model update` 和 `model chunk` 是模型 OTA 的子协议（见第 4 节）
- C6 解析 `CMD:` 时只取第一个 `:` 后的第一个 `:` 作为设备和命令的分界，命令内部可以包含冒号

### 2.5 直发命令（旧版，C6 会报 unknown）

```
model list\n
model list_ack\n
```

- 这些命令没有 `CMD:<name>:` 前缀
- C6 v2.2 不会识别它们，会回复 `{"error":"unknown","cmd":"..."}`
- P4 在配对后已改用 `CMD:<name>:model list` 格式，仅开机首次可能发送旧格式

---

## 3. C6 → P4 上行消息格式

### 3.1 开机通知

C6 上电后立即发送：

```json
{"bridge":"ready","version":"2.1"}
```

### 3.2 周期心跳（必填！）

P4 要求 C6 定期发送心跳消息，**这是当前 C6 缺失的功能**。

```json
{"bridge":"heartbeat","ble":"up","age":0,"listen":2}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `bridge` | string | 固定为 `"heartbeat"` |
| `ble` | string | BLE 状态（可选，目前仅用于日志） |
| `age` | int | 距上次收到数据的秒数 |
| `listen` | int | C6 当前监听设备数 |

**间隔：** 建议每 5~10 秒发送一次。

**为什么需要心跳？**

- P4 用 `listen` 字段检测 C6 监听数与自身记录的配对数量是否一致（用于 C6 重启后的自愈）
- P4 用 `age` 字段判断链路是否存活，超过 90 秒无心跳则视为 C6 离线
- 缺少心跳会导致 P4 的 `uart_receiver_is_connected()` 始终返回 false，影响模型上传和云同步等依赖连接状态的逻辑

### 3.3 监听确认

```json
{"listen":"ok","name":"S3_03"}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `listen` | string | `"ok"` 表示成功 |
| `name` | string | 设备名 |
| `mac` | string | 可选，如果已在 listen 时已知 MAC 则带上（格式: `"A8:03:2A:E1:00:04"`） |

错误情况：

```json
{"error":"empty_name"}
{"error":"full"}
```

### 3.4 取消确认

```json
{"cancel":"ok","name":"S3_03"}
{"cancel":"not_found"}
{"cancel":"all"}
```

### 3.5 MAC 学习通知（重要！需修改格式）

当前 C6 发送：

```json
{"learned":"S3_03","mac":"A8:03:2A:E1:00:04"}
```

**P4 解析代码要求的格式：**

```json
{"name":"S3_03","mac":"A8:03:2A:E1:00:04"}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | **设备名（关键！C6 当前用的是 `learned`，但 P4 只认 `name`）** |
| `mac` | string | MAC 地址，格式 `"XX:XX:XX:XX:XX:XX"` |

**这是导致 P4 能发指令但收不到 C6 消息的核心原因之一。** C6 用 `learned` 作为 key，但 P4 的 MAC 学习回调只查找 `name` 字段。请 C6 将 key 改为 `name`。

### 3.6 CMD 下发结果

```json
{"cmd":"ok","name":"S3_03","to":"uv_on"}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `cmd` | string | `"ok"` 表示 ESP-NOW 发送成功 |
| `name` | string | 目标设备名 |
| `to` | string | 实际下发的命令内容 |

错误：

```json
{"error":"send_fail","name":"S3_03"}
{"error":"not_found","name":"S3_03"}
{"error":"cmd_format"}
```

### 3.7 未知命令

```json
{"error":"unknown","cmd":"model list"}
```

- 收到此消息表示 C6 无法识别的命令格式
- 对于模型类命令（`model update`、`model chunk`），C6 不需要解析内容，只需确保通过 `CMD:<name>:` 包装后，C6 将其转发给目标设备

### 3.8 传感器数据透传

C6 将 ESP-NOW 收到的数据**不加修改直接写入 UART**，并追加 `\n`：

```
{"name":"S3_03","t":27.1,"hu":69.9,"o":5.5,"h":0.1,"c":16.7,"v":5.6,"co2":659,"cls":"air","conf":0.00,"fr":0,"uv":0,...}
```

**P4 解析的 JSON 字段表：**

| P4 字段 | JSON key (主) | JSON key (备选) |
|---------|---------------|-----------------|
| 设备名 | `node` | `name` |
| 温度 | `t` | `temp` |
| 湿度 | `hu` | `hum` |
| 气味 | `o` | `odor` |
| 甲醛 | `h` | `hcho` |
| CO | `c` | `co` |
| VOC | `v` | `voc` |
| CO2 | `co2` | — |
| 传感器类型 | `cls` | `sensor_class` |
| 置信度 | `conf` | — |
| 新鲜度 | `fr` | `fresh` |
| UV 开关 | `uv` | — |
| UV 自动 | `ua` | `uv_auto` |
| UV 剩余 | `ur` | `uv_remain` |
| 雾化开关 | `fo` | `fog` |
| 雾化自动 | `fa` | `fog_auto` |
| 风扇开关 | `fn` | `fan` |
| 风扇自动 | `fl` | `fan_auto` |
| 舱门开关 | `lo` | `lid` |
| 舱门自动 | `la` | `lid_auto` |
| 舱门剩余 | `lr` | `lid_remain` |
| 模型就绪 | `mr` | `model_ready` |
| 当前模型名 | `mn` | — |
| 模型版本 | `mv` | — |
| 模型大小 | `ms` | — |

**重要：** C6 无需修改传感器 JSON 内容，直接透传即可。

### 3.9 模型 OTA 响应透传

S3 在模型升级过程中的响应也通过 C6 直接透传：

```json
{"mt":"chunk_ok","mid":0}
{"mt":"model_done"}
{"mt":"model_fail"}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `mt` | string | 消息类型：`chunk_ok` / `model_done` / `model_fail` |
| `mid` | int | 确认的块编号 |

C6 不需要识别这些消息，直接透传即可。P4 解析 `mt` 字段驱动状态机。

---

## 4. 模型 OTA 升级流程

```
P4                                      C6                           S3
──                                      ──                           ──
1. 从云端下载 .tflite 到 PSRAM
   保存到 SD 卡 /sdcard/model/xxx.tflite

2. CMD:设备名:model update <name> <chunks> <size> <ver>
   ─────────────────────────────────────────►   ESP-NOW 转发
                                                 ─────────────►  解析元数据
                                                                 准备接收

3.                                                 ◄─────────────  {"mt":"chunk_ok","mid":0}
   ◄──────────────────────────────────────────  透传

4. CMD:设备名:model chunk <id> <hex_data>
   ─────────────────────────────────────────►   ESP-NOW 转发
                                                 ─────────────►  接收块数据
                                                                 写入 Flash

5.                                                 ◄─────────────  {"mt":"chunk_ok","mid":0}
   ◄──────────────────────────────────────────  透传

   重复步骤 4-5，直到所有块发送完毕

6.                                                 ◄─────────────  {"mt":"model_done"}
   ◄──────────────────────────────────────────  透传

   P4 更新本地记录，触发云端上报

   或失败：
                                                 ◄─────────────  {"mt":"model_fail"}
```

### 参数说明

- `name`: 模型名（如 `food_freshness`）
- `chunks`: 总块数
- `size`: 总字节数
- `ver`: 版本号（如 `1.0.0`）
- `id`: 块序号（从 0 开始）
- `hex_data`: 100 字节模型数据的十六进制编码（200 个 hex 字符）

### 超时与重试

- 每步超时：5 秒（`MODEL_CMD_TIMEOUT`）
- 最大重试：3 次
- 重试策略：超时后重发当前命令（元数据或当前块）

---

## 5. 已知问题与修复清单

### 🔴 问题 1：C6 未发送心跳

**现象：** P4 在无传感器数据约 90 秒后认为 C6 离线。

**修复：** C6 每 5~10 秒发送：

```json
{"bridge":"heartbeat","listen":2}
```

`listen` 字段必须是 C6 当前的真实监听数。

### 🔴 问题 2：MAC 学习消息 key 错误

**当前 C6 发送：** `{"learned":"S3_03","mac":"..."}`

**P4 要求：** `{"name":"S3_03","mac":"..."}`

**修复：** C6 将 JSON key 从 `learned` 改为 `name`。

### 🟡 问题 3：`model list` 使用旧格式（低优先）

P4 在某些启动路径下仍然发送裸 `model list\n`，C6 应忽略或兼容。

### 🟡 问题 4：透传数据可能跨行

C6 使用 `Serial1.write(data, len)` + `Serial1.println()` 透传。如果 S3 发送的数据中包含 `\n`，P4 的行分割会断裂。建议 S3 和 C6 都确保 JSON 数据为单行。

---

## 6. 调试建议

1. C6 可通过 `Serial.printf()` 输出调试信息（仅 USB 串口，不影响 `Serial1` 与 P4 的通信）
2. 每次 LISTEN 后，C6 发送 `{"listen":"ok","name":"xxx"}`，P4 收到后会打印日志
3. 每次 CMD 转发后，C6 发送 `{"cmd":"ok","name":"xxx","to":"xxx"}`，P4 会打印 `CMD -> ...`
4. P4 日志中 `RAW UART from C6` 前缀的行是 C6 透传的原始数据
5. C6 可通过 `STATUS` 命令获取统计信息，P4 收到 `{"status":"ok",...}` 但不做处理，仅调试用

---

## 7. 快速参考：C6 需要实现的完整消息集

| 方向 | 消息 | 必须实现 |
|------|------|---------|
| P4→C6 | `LISTEN:<name>` | ✅ 已有 |
| P4→C6 | `CANCEL:<name>` | ✅ 已有 |
| P4→C6 | `CANCEL ALL` | ✅ 已有 |
| P4→C6 | `CMD:<name>:<cmd>` | ✅ 已有 |
| C6→P4 | `{"bridge":"ready"}` | ✅ 已有 |
| C6→P4 | 周期心跳 `{"bridge":"heartbeat","listen":N}` | ❌ 缺失 |
| C6→P4 | `{"listen":"ok","name":"..."}` | ✅ 已有 |
| C6→P4 | `{"cancel":"ok",...}` | ✅ 已有 |
| C6→P4 | `{"name":"...","mac":"..."}` (MAC 学习) | ❌ key 错误，需改为 `name` |
| C6→P4 | `{"cmd":"ok","name":"...","to":"..."}` | ✅ 已有 |
| C6→P4 | `{"error":"..."}` | ✅ 已有 |
| C6→P4 | 透传传感器 JSON | ✅ 已有 |
| C6→P4 | 透传模型 OTA 响应 | ✅ 已有（依赖 MAC 学习修正） |

---

*文档版本：v1.0 / 2026-06-24*
