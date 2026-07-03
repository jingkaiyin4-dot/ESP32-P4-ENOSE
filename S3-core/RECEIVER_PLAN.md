# S3 Receiver 接收端开发计划书

> 项目：AIoT果蔬新鲜度监测系统 | 版本：v2.0 | 日期：2026-06-30

---

## 1. 项目概述

### 1.1 系统定位

ESP32-S3 **接收端**是AIoT果蔬新鲜度监测系统的**核心智能节点**，负责：

- 接收Sender发送的传感器数据（ESP-NOW）
- 本地运行TFLite模型推理新鲜度分类
- 上行转发数据到P4网关（经由C6 Bridge）
- 下行接收P4控制命令（UV/盖子/雾化/风扇）
- OTA模型更新（ESP-NOW分片传输→PSRAM缓冲→SD卡原子写入）

### 1.2 系统架构

```
Sender(S3) ──ESP-NOW──→ Receiver(S3) ──ESP-NOW──→ C6 Bridge ──UART──→ P4 Gateway ──HTTP──→ 云端
                                                                        ↓
                                                                  控制命令下发
                                                                        ↓
P4 ──UART──→ C6 ──ESP-NOW──→ Receiver(S3) ──GPIO──→ UV/盖子/雾化/风扇
```

### 1.3 硬件规格

| 项目 | 规格 |
|------|------|
| 芯片 | ESP32-S3-WROOM-1-N16R8 |
| Flash | 16MB |
| PSRAM | 8MB (OPI) |
| SD卡 | SPI模式 (SCK=18, MISO=14, MOSI=13, CS=15) |
| 本机MAC | A8:03:2A:E1:00:02 |
| 开发板 | XIAO ESP32-S3 |

---

## 2. 当前代码状态

### 2.1 模块架构（4503行，12个文件）

| 模块 | 行数 | 功能 | 状态 |
|------|------|------|------|
| `config.h` | 222 | 全局常量、引脚定义、extern声明 | ✅ 稳定 |
| `espnow-receiver-s3.ino` | 332 | 主循环：数据处理→推理→上报→控制 | ✅ 已修改(越界保护) |
| `ble_manager.h` | 359 | ESP-NOW收发、数据环缓冲、转发P4 | ✅ 已修改(越界保护) |
| `sd_manager.h` | 601 | SD卡初始化、模型扫描/加载、norm加载、类名管理 | ✅ 已修改(维度安全检查) |
| `model_runner.h` | 200 | TFLite推理、新鲜度评分、空气基准 | ✅ 稳定 |
| `model_updater.h` | 771 | OTA模型更新：5态状态机、PSRAM缓冲、原子写入 | ✅ 稳定 |
| `relay_control.h` | 669 | UV继电器控制(GPIO1)+自动模式+心跳检测 | ✅ 稳定 |
| `servo_control.h` | 133 | 盖子舵机(GPIO4)+自动换气 | ✅ 稳定 |
| `atomization.h` | 93 | 雾化加湿器(GPIO3)+脉冲触发+湿度自动 | ✅ 稳定 |
| `fan_control.h` | 89 | 12V风扇(3V继电器GPIO5)+新鲜度自动 | ✅ 稳定 |
| `serial_cmd.h` | 246 | 串口命令调度（调试/模型管理） | ✅ 稳定 |
| `display_manager.h` | 514 | TFT显示+QR码（已禁用，SPI冲突） | ⚠️ 禁用 |
| `web_server.h` | 274 | WiFi HTTP OTA（已禁用，改ESP-NOW OTA） | ⚠️ 禁用 |

### 2.2 本次修改（2026-06-30）

**类名越界保护** — 3个文件，解决`general_rotten`误报：

| 文件 | 修改点 | 说明 |
|------|--------|------|
| `sd_manager.h` L284-306 | `loadModelByFile()` 安全检查 | 模型输出维度vs类名数不匹配时截断/补齐，打印`[CLASSES] Final`诊断 |
| `ble_manager.h` L275-282 | `sendToP4()` 越界保护 | `mainClassName`默认值改为`"unknown"`，仅模型加载+类名有效时取真实值 |
| `espnow-receiver-s3.ino` L198+L221 | 两处`classNames[]`访问 | 加`predClass >= 0 && predClass < numClasses`范围检查 |

---

## 3. 已知问题与解决计划

### 3.1 P0级（阻塞功能）

| # | 问题 | 根因 | 解决方案 | 状态 |
|---|------|------|----------|------|
| P0-1 | SD卡偶尔识别失败 | `new SPIClass()`默认HSPI，PSRAM报错`0x00ffffff`但可能间接影响 | 硬件排查：插拔SD卡、换卡、确认FAT32 | 待用户验证 |
| P0-2 | 推理输出`general_rotten` | norm.json未加载→回退默认13类→index=10映射错误 | 本次代码修复+OTA推送正确norm.json | 代码已修复，OTA待推送 |
| P0-3 | 模型类名不匹配 | SD卡模型可能为旧14类版本，norm.json不存在 | OTA推送`banana_air_v2`模型+norm.json | 待OTA |

### 3.2 P1级（影响体验）

| # | 问题 | 根因 | 解决方案 | 状态 |
|---|------|------|----------|------|
| P1-1 | TFT显示禁用 | `display_manager.h`使用GPIO18(SCK)与SD卡冲突 | 方案A: TFT改FSPI驱动；方案B: 暂不恢复 | 待决策 |
| P1-2 | 模型类别显示class_N占位 | norm.json缺失时补齐类名用`class_N`格式 | OTA推送正确norm.json根治 | 待OTA |
| P1-3 | PSRAM ID read error | ESP32-S3 OPI PSRAM初始化偶发报错，不阻塞程序 | 已有容错处理（尝试esp_psram_init备选） | 已处理 |

### 3.3 P2级（优化项）

| # | 问题 | 根因 | 解决方案 | 状态 |
|---|------|------|----------|------|
| P2-1 | display_manager.h SPI冲突 | GPIO18同时给TFT_SCK和SD_SCK | TFT改用其他SPI引脚或软件SPI | 暂禁用 |
| P2-2 | 多台S3广播命令重复 | C6 v2.4广播时所有同信道设备收到 | MAC学习完成后自动精确发送 | 低优先 |
| P2-3 | listenList重启丢失 | C6 NVS持久化未实现 | 每次开机重新LISTEN配对 | 低优先 |

---

## 4. 开发里程碑

### Phase 1: 基础功能验证（当前）

- [x] ESP-NOW数据接收+环缓冲
- [x] TFLite模型推理
- [x] P4上行数据转发（经C6 Bridge）
- [x] UV/盖子/雾化/风扇本地自动控制
- [x] 串口命令调试
- [x] OTA模型更新（5态状态机）
- [x] 类名越界保护
- [ ] **烧录验证** — 确认`[CLASSES] Final`输出
- [ ] **OTA推送** — `banana_air_v2`模型+norm.json到SD卡
- [ ] **SD卡稳定性** — 确认长期运行不掉卡

### Phase 2: 完整链路测试

- [ ] Sender→Receiver→C6→P4→云端 全链路数据流
- [ ] P4→C6→Receiver 下行命令响应延迟 < 2秒
- [ ] 模型OTA从云端→P4→C6→Receiver全流程
- [ ] 多水果场景测试（空气/香蕉新鲜/香蕉变质）

### Phase 3: 优化与稳定性

- [ ] TFT显示恢复（解决SPI冲突）
- [ ] P4心跳检测优化（从sensor_data.json timestamp推算）
- [ ] SD卡日志CSV旋转优化
- [ ] 长期运行稳定性测试（24小时+）

### Phase 4: 比赛交付

- [ ] 全系统Demo录制
- [ ] 技术文档整理
- [ ] 代码注释完善

---

## 5. SD卡引脚配置（PCB固定，不可改）

| 信号 | GPIO | SPI功能 | 备注 |
|------|------|---------|------|
| SCK | 18 | 时钟 | 与TFT_SCK冲突（TFT已禁用） |
| MISO | 14 | 主入从出 | |
| MOSI | 13 | 主出从入 | |
| CS | 15 | 片选 | |

`initSD()`使用`new SPIClass()`默认HSPI(SPI3)，之前一直能读SD卡。PSRAM占用HSPI但不完全阻塞SPI操作。

---

## 6. 外设控制引脚

| 外设 | GPIO | 驱动方式 | 自动触发条件 |
|------|------|----------|-------------|
| UV继电器 | 1 (D1) | NC型继电器，LOW=通电 | 新鲜度≤60%→自动开30秒 |
| 盖子舵机 | 4 (D2) | PWM 500/2450μs | 新鲜度≤40%→自动开盖60秒 |
| 雾化加湿 | 3 | 脉冲触发模式 | 湿度<40%→自动雾化 |
| 风扇 | 5+3V继电器 | GPIO HIGH=风扇转 | 新鲜度≤50%→自动排风 |

---

## 7. 待烧录清单

| 设备 | 固件 | 修改内容 | 优先级 |
|------|------|----------|--------|
| **S3 Receiver** | espnow-receiver-s3.ino v2.0 | 类名越界保护(3文件) | **最高** |
| S3 Sender | espnow-sender-s3.ino v1.2 | GAS_SENSORS_PRESENT+MHZ修复+WiFi禁用 | 高 |
| C6 Bridge | ESP-C6-bridge.ino v2.5 | USB镜像+usbConnected | 已烧录 |

---

## 8. 烧录后验证步骤

1. 打开串口监视器 115200bps
2. 观察启动日志：
   - `SD OK` 或 `[WARN] SD card failed`
   - `[CLASSES] Final: N classes:` + 类名列表
   - `Model loaded OK: /model.tflite`
3. 等待ESP-NOW数据到达
4. 观察推理输出：`-> Type:xxx State:xxx Conf:xx% Fresh:xx/100`
5. 确认不再出现`general_rotten`
6. 测试串口命令：`status`, `models`, `uv on/off`

---

*文档生成：2026-06-30 | 作者：AI工程师*
