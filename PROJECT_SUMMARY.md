# ESP32-P4 电子鼻系统 (E-Nose System)

> AI友好的项目文档 - 用于项目交接和AI助手理解

---

## 项目概述

这是一个基于 **ESP32-P4 Function EV Board** 的电子鼻多节点传感器系统，主要功能是：
1. 通过摄像头扫描二维码识别传感器节点
2. 显示环境温湿度/气压数据
3. 模拟显示VOC、CO2、Ethylene等气体数据

---

## 硬件配置

| 组件 | 型号 | 备注 |
|------|------|------|
| 主控 | ESP32-P4 Function EV Board | 240MHz, 16MB Flash, 8MB PSRAM |
| 显示屏 | 7" MIPI-DSI | 1024x600 |
| 摄像头 | SC2336 MIPI-CSI | 1280x720 @ 30FPS |
| 环境传感器 | BME680 (Waveshare) | I2C接口 |

---

## 引脚接线

### BME680 环境传感器
```
BME680      ESP32-P4
VCC    ────► 3.3V
GND    ────► GND
SCL    ────► GPIO 8
SDA    ────► GPIO 7
ADDR   ────► GND (地址0x76) 或 悬空 (地址0x77)
CS     ────► NC (不接)
```

### 摄像头
- 使用板载 MIPI-CSI 接口，无需额外接线

---

## 功能模块

### 1. 摄像头二维码扫描
- **位置**: `main/ui.cpp` - `camera_frame_cb()`
- **分辨率**: 1280x720 → 缩放到 200x150
- **帧率**: 摄像头30FPS，显示每2帧更新
- **二维码库**: quirc
- **显示优化**: 双缓冲防止撕裂

### 2. 二维码解析
- **位置**: `main/ui.cpp` - QR处理代码块
- **支持的二维码内容**:
  - `S3A`, `S3_B`, `S3A`, `S3B`, `S3`
- **显示**: 节点ID + 模拟的VOC/CO2/ETH数据

### 3. BME680 环境传感器
- **位置**: `components/bme680_sensor/`
- **I2C地址**: 0x77 (默认)
- **读取周期**: 每2秒
- **数据**: 温度(°C)、湿度(%)、气压(hPa)

### 4. UI界面
- **位置**: `main/ui.cpp` - `ui_init()`
- **界面**:
  - 启动界面: WiFi连接 + 进入按钮
  - 主仪表盘: 环境数据 + 节点面板
  - 二维码扫描: 摄像头画面 + 扫描框

---

## 文件结构

```
esp_brookesia_phone/
├── main/
│   ├── main.cpp          # 主程序, BME680读取任务
│   ├── ui.cpp            # LVGL界面, 摄像头回调, 二维码处理
│   ├── app_video.c       # 摄像头驱动 (ESP官方库)
│   ├── CMakeLists.txt    # CMake配置
│   └── ui.h              # UI头文件
├── components/
│   ├── bme680_sensor/    # BME680驱动组件
│   │   ├── bme680.c      # 驱动实现
│   │   ├── bme680.h      # 头文件
│   │   └── CMakeLists.txt
│   └── bsp_extra/        # 板级支持组件
└── sdkconfig           # ESP-IDF配置
```

---

## 关键代码位置

### BME680初始化
```cpp
// main/main.cpp
bme680_init(i2c_bus);  // 在 bme680_task() 中调用
```

### BME680数据读取
```cpp
// main/main.cpp - bme680_task()
while (1) {
    bme680_read(&g_temperature, &g_humidity, &g_pressure);
    vTaskDelay(pdMS_TO_TICKS(2000));
}
```

### 二维码处理
```cpp
// main/ui.cpp - camera_frame_cb() 中的QR代码块
// 位置: ~第130-230行
```

### UI更新
```cpp
// main/ui.cpp - fake_data_timer_cb()
// 位置: ~第270行
// 更新BME680显示和环境传感器数据
```

---

## 已知问题

### ⚠️ BME680 数据不准
- **现象**: 温度/湿度/气压数值与实际有偏差
- **可能原因**:
  1. 计算公式需要Bosch官方标定系数
  2. 建议使用BSEC库进行温度补偿
  3. 需要校准湿度传感器的H1/H2系数
- **建议**: 参考官方示例使用完整的BSEC库

### 待优化项
- [ ] BME680数据补偿算法
- [ ] 二维码识别速度
- [ ] 图像旋转支持（用户壳体摄像头朝左）
- [ ] S3节点实际数据通信（目前是模拟）

---

## 开发命令

### 编译
```bash
cd D:/ESP32-P4/esp_brookesia_phone
idf.py build
```

### 烧录
```bash
idf.py -p COMXX flash
```

### 监控串口
```bash
idf.py monitor
```

---

## 技术栈

| 技术 | 版本 |
|------|------|
| ESP-IDF | v5.5 |
| LVGL | v9 |
| FreeRTOS | 内置 |
| 摄像头驱动 | ESP-Video (V4L2) |
| 二维码库 | quirc |

---

## AI助手指南

### 如果要修改BME680驱动
1. 参考 `components/bme680_sensor/bme680.c`
2. 官方寄存器定义在 Waveshare BME68X 示例代码中
3. 数据格式: 0x1D寄存器开始，共17字节
4. 温度/湿度/气压需要使用校准系数

### 如果要修改UI
1. 主UI在 `main/ui.cpp` 的 `ui_init()` 函数
2. LVGL对象创建顺序影响层级（后创建在上层）
3. 定时更新在 `fake_data_timer_cb()` 中

### 如果要修改摄像头
1. 摄像头驱动在 `main/app_video.c`
2. 分辨率在 sdkconfig 中配置
3. 帧回调在 `ui.cpp` 的 `camera_frame_cb()`

---

**最后更新**: 2026-03-29
