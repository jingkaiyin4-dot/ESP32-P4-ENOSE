/**
 * display_manager.h — ST7789 240x280 TFT 显示 + QR码配对
 * 
 * 硬件: 1.69寸 IPS ST7789 SPI屏 240x280
 * 功能:
 *   - 开机/配对模式: 显示BLE配对QR码 + 设备名
 *   - 运行模式: 显示传感器数据、新鲜度、UV状态
 *   - RTOS独立任务渲染，不阻塞主循环
 * 
 * RTOS架构:
 *   - displayTask (Core 1, 优先级1, 4KB栈): 每500ms刷新屏幕
 *   - QR码生成一次性完成(~5ms)，结果缓存在PSRAM
 *   - 使用 FreeRTOS 互斥锁保护显示状态
 *   - SPI总线与SD卡共享，已通过SPI.beginTransaction保护
 * 
 * TFT引脚 (XIAO ESP32-S3):
 *   TFT_SCK  = D8  (GPIO18)
 *   TFT_MOSI = D10 (GPIO21)
 *   TFT_CS   = D9  (GPIO17)
 *   TFT_DC   = D3  (GPIO6)
 *   TFT_RST  = D2  (GPIO5)
 *   TFT_BL   = D4  (GPIO7)  背光
 * 
 * 配对逻辑:
 *   1. 开机默认进入配对模式，屏幕显示QR码
 *   2. P4扫描QR码获取BLE Service UUID + 设备名
 *   3. P4连接BLE后自动退出配对模式进入运行模式
 *   4. 串口命令 "qr show"/"qr hide" 手动切换
 *   5. P4 BLE命令 {"cmd":"qr_show"}/{"cmd":"qr_hide"}
 * 
 * QR码内容: JSON格式
 *   {"name":"S3-E-Nose","svc":"ebe0ccb0-7a0a-4b0c-8a1a-6ffed7a7caaa","mac":"XX:XX:XX:XX:XX:XX"}
 */
#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "config.h"
#include <WiFi.h>
#include <TFT_eSPI.h>
#include "qrcode_lib.h"  // 本地副本，避免ESP-IDF内置qrcode.h冲突

// ==================== TFT 引脚定义 (XIAO ESP32-S3) ====================
// 在 User_Setup.h 中配置，这里仅做注释
// 实际引脚映射见 tft_user_setup.h

// ==================== 显示状态 ====================
enum DisplayMode {
    DISP_MODE_PAIRING = 0,   // 配对模式: QR码 + 设备名
    DISP_MODE_RUNNING = 1,   // 运行模式: 数据面板
    DISP_MODE_WARMUP  = 2,   // 预热模式: 进度条
    DISP_MODE_ERROR   = 3,   // 错误模式: 错误信息
};

// ==================== 显示状态结构 ====================
struct DisplayState {
    DisplayMode mode;
    // 运行模式数据
    char   predClass[32];
    float  confidence;
    int    freshness;
    float  odor, hcho, co, voc;
    uint16_t co2;
    float  temp, humidity;
    bool   uvOn;
    bool   uvAuto;
    unsigned long uvRemain;
    // 配对模式
    bool   bleConnected;
    char   bleMac[20];
    // 状态变更标志
    volatile bool dirty;
};

// ==================== 全局实例 ====================
TFT_eSPI     tft = TFT_eSPI(240, 280);  // 240x280 严格分辨率
DisplayState dispState;
SemaphoreHandle_t dispMutex = nullptr;
TaskHandle_t displayTaskHandle = nullptr;

// QR码缓存 (一次性生成，反复使用)
#define QR_VERSION 3  // version 3 = 29x29模块, 足够编码配对JSON
// 缓冲区大小: ((29*29)+7)/8 = 106 bytes (qrcode_getBufferSize计算结果)
#define QR_BUFFER_SIZE 106
static uint8_t* qrModules = nullptr;  // 动态分配(PSRAM优先)
static QRCode  qrCode;
static bool    qrGenerated = false;

// ==================== 颜色定义 ====================
#define COLOR_BG         TFT_BLACK
#define COLOR_TEXT       TFT_WHITE
#define COLOR_ACCENT     0x07FF  // 青色
#define COLOR_GOOD       0x07E0  // 绿色
#define COLOR_WARN       0xFFE0  // 黄色
#define COLOR_BAD        0xF800  // 红色
#define COLOR_UV         0x801F  // 紫色
#define COLOR_QR_BLACK   TFT_BLACK
#define COLOR_QR_WHITE   TFT_WHITE
#define COLOR_DIM        0x4208  // 暗灰

// ==================== 前向声明 ====================
void displayTask(void* arg);
void drawPairingScreen();
void drawRunningScreen();
void drawWarmupScreen(uint16_t remaining);
void drawErrorScreen(const char* msg);
void generateQRCode();
int  mapFreshnessToColor(int freshness);

// ==================== 初始化 ====================
void initDisplay() {
    // 初始化互斥锁
    dispMutex = xSemaphoreCreateMutex();
    if (!dispMutex) {
        Serial.println("[DISP] FATAL: mutex creation failed!");
        return;
    }
    
    Serial.printf("[DISP] Heap before TFT init: %d free, %d PSRAM free\n", 
                  ESP.getFreeHeap(), ESP.getFreePsram());
    
    // 初始化TFT
    // 先手动初始化SPI总线，确保引脚正确分配
    SPI.begin(18, -1, 21, 17);  // SCLK=18, MISO=-1, MOSI=21, SS=17
    Serial.println("[DISP] SPI.begin() done");
    tft.init();
    Serial.println("[DISP] tft.init() done");
    tft.setRotation(2);  // 竖屏 240x280 (根据实际方向调整 0/2)
    tft.fillScreen(COLOR_BG);
    tft.setTextDatum(MC_DATUM);  // 中中对齐
    
    // 开启背光
    pinMode(7, OUTPUT);   // TFT_BL = D4 = GPIO7
    digitalWrite(7, HIGH);
    
    // 显示启动画面
    tft.setTextColor(COLOR_ACCENT);
    tft.setTextSize(2);
    tft.drawString("S3-E-Nose", 120, 120);
    tft.setTextSize(1);
    tft.setTextColor(COLOR_DIM);
    tft.drawString("Initializing...", 120, 150);
    
    // 初始化显示状态
    memset(&dispState, 0, sizeof(dispState));
    dispState.mode = DISP_MODE_PAIRING;
    dispState.dirty = true;
    
    Serial.printf("[DISP] Heap after TFT init: %d free\n", ESP.getFreeHeap());
    
    // 生成QR码
    generateQRCode();
    
    Serial.printf("[DISP] Heap after QR gen: %d free\n", ESP.getFreeHeap());
    
    // 创建显示任务 (Core 1, 优先级1, 不影响BLE和推理)
    BaseType_t taskResult = xTaskCreatePinnedToCore(
        displayTask,
        "dispTask",
        6144,       // 6KB栈 (TFT_eSPI drawString需要较多栈)
        NULL,
        1,          // 优先级1 (低于BLE和推理)
        &displayTaskHandle,
        1           // Core 1
    );
    
    if (taskResult != pdPASS) {
        Serial.printf("[DISP] FATAL: displayTask creation failed (err=%d, freeHeap=%d)\n", 
                      taskResult, ESP.getFreeHeap());
    } else {
        Serial.println("[DISP] ST7789 240x280 initialized, display task started on Core 1");
    }
}

// ==================== 生成QR码 (一次性) ====================
void generateQRCode() {
    // 构造配对JSON
    char qrText[128];
    snprintf(qrText, sizeof(qrText),
             "{\"name\":\"%s\",\"svc\":\"%s\"}",
             BLE_DEVICE_NAME, BLE_SVC_UUID);
    
    Serial.printf("[QR] Generating: %s\n", qrText);
    
    // 分配QR码缓冲区 (优先PSRAM, fallback到内部RAM)
    uint16_t bufSize = qrcode_getBufferSize(QR_VERSION);
    Serial.printf("[QR] Buffer size needed: %u bytes\n", bufSize);
    
    if (ESP.getFreePsram() > bufSize + 1024) {
        qrModules = (uint8_t*)ps_malloc(bufSize);
        Serial.printf("[QR] Allocated from PSRAM: %p\n", qrModules);
    } else {
        Serial.println("[QR] PSRAM too small or not available, using heap");
    }
    
    if (!qrModules) {
        qrModules = (uint8_t*)malloc(bufSize);
        Serial.printf("[QR] Allocated from heap: %p\n", qrModules);
    }
    
    if (!qrModules) {
        Serial.println("[QR] FATAL: Buffer allocation FAILED");
        qrGenerated = false;
        return;
    }
    
    // 清零缓冲区以防野数据
    memset(qrModules, 0, bufSize);
    
    uint32_t dt = millis();
    int8_t result = qrcode_initText(&qrCode, qrModules, QR_VERSION, ECC_MEDIUM, qrText);
    dt = millis() - dt;
    
    if (result == 0) {
        qrGenerated = true;
        Serial.printf("[QR] Generated v%d (%dx%d) in %lums, buffer at %p\n",
                      qrCode.version, qrCode.size, qrCode.size, dt, qrModules);
    } else {
        qrGenerated = false;
        Serial.printf("[QR] Generation FAILED (err=%d)\n", result);
    }
}

// ==================== 显示任务 (RTOS) ====================
void displayTask(void* arg) {
    // 配对模式下的闪烁计数
    static unsigned long lastDrawMs = 0;
    static bool blinkState = false;
    
    while (true) {
        unsigned long now = millis();
        
        // 500ms刷新间隔 (2fps, 足够且省电)
        if (now - lastDrawMs < 500) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        lastDrawMs = now;
        blinkState = !blinkState;
        
        // 获取互斥锁
        if (xSemaphoreTake(dispMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        DisplayMode mode = dispState.mode;
        bool dirty = dispState.dirty;
        dispState.dirty = false;
        
        xSemaphoreGive(dispMutex);
        
        // 只在dirty或特定模式下重绘
        // 配对模式: 每次都重绘(闪烁效果)
        // 运行模式: 仅dirty时重绘
        if (!dirty && mode != DISP_MODE_PAIRING) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        switch (mode) {
            case DISP_MODE_PAIRING:
                drawPairingScreen();
                break;
            case DISP_MODE_RUNNING:
                drawRunningScreen();
                break;
            case DISP_MODE_WARMUP:
                // drawWarmupScreen();  // 暂未实现
                break;
            case DISP_MODE_ERROR:
                // drawErrorScreen();   // 暂未实现
                break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ==================== 配对模式: QR码屏幕 ====================
void drawPairingScreen() {
    tft.fillScreen(COLOR_BG);
    
    // ---- 标题 ----
    tft.setTextDatum(TC_DATUM);  // 顶部居中
    tft.setTextColor(COLOR_ACCENT);
    tft.setTextSize(1);
    tft.drawString("Scan to Pair", 120, 8);
    
    // ---- QR码 (居中) ----
    if (qrGenerated) {
        // QR码尺寸计算
        // version 3 = 29x29 模块
        // 屏幕宽240, 留边距, 最大显示宽度 = 200px
        // 每模块像素 = 200 / 29 = 6.89 → 取6, 总宽 = 29*6 = 174px
        int scale = 6;
        int qrWidth = qrCode.size * scale;   // 174px
        int offsetX = (240 - qrWidth) / 2;   // 居中: (240-174)/2 = 33
        int offsetY = 28;                      // 标题下方
        
        // 绘制白色背景 (留4模块安静区)
        int quietZone = 4;
        int totalWidth = (qrCode.size + quietZone * 2) * scale;
        int bgOffsetX = (240 - totalWidth) / 2;
        tft.fillRect(bgOffsetX, offsetY - quietZone * scale,
                     totalWidth, totalWidth + quietZone * scale,
                     COLOR_QR_WHITE);
        
        // 逐模块绘制QR码
        for (int y = 0; y < qrCode.size; y++) {
            for (int x = 0; x < qrCode.size; x++) {
                bool isBlack = qrcode_getModule(&qrCode, x, y);
                int px = offsetX + x * scale;
                int py = offsetY + y * scale;
                if (isBlack) {
                    tft.fillRect(px, py, scale, scale, COLOR_QR_BLACK);
                }
                // 白色模块已在背景填充
            }
        }
        
        // ---- 设备名 (QR码下方) ----
        int textY = offsetY + qrWidth + 10;
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(COLOR_TEXT);
        tft.setTextSize(2);
        tft.drawString(BLE_DEVICE_NAME, 120, textY);
        
        // ---- BLE状态 ----
        tft.setTextSize(1);
        if (dispState.bleConnected) {
            tft.setTextColor(COLOR_GOOD);
            tft.drawString("BLE Connected", 120, textY + 24);
        } else {
            // 闪烁效果
            if (millis() % 1000 < 600) {
                tft.setTextColor(COLOR_WARN);
                tft.drawString("Waiting for P4...", 120, textY + 24);
            }
        }
        
        // ---- MAC地址 ----
        tft.setTextColor(COLOR_DIM);
        tft.drawString(WiFi.macAddress(), 120, textY + 42);
        
    } else {
        // QR码生成失败
        tft.setTextColor(COLOR_BAD);
        tft.setTextSize(1);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("QR Code Error", 120, 100);
        tft.drawString("Use BLE to connect", 120, 120);
    }
    
    // ---- 底部提示 ----
    tft.setTextColor(COLOR_DIM);
    tft.setTextSize(1);
    tft.setTextDatum(BC_DATUM);
    tft.drawString("S3-E-Nose v1.0", 120, 276);
}

// ==================== 运行模式: 数据面板 ====================
void drawRunningScreen() {
    tft.fillScreen(COLOR_BG);
    
    // ---- 顶部: 预测结果 + 新鲜度 ----
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COLOR_ACCENT);
    tft.setTextSize(1);
    tft.drawString("PREDICTION", 8, 6);
    
    // 预测类别
    tft.setTextColor(COLOR_TEXT);
    tft.setTextSize(2);
    tft.drawString(dispState.predClass, 8, 22);
    
    // 新鲜度圆弧 (简化为数字+颜色)
    int fresh = dispState.freshness;
    int color = mapFreshnessToColor(fresh);
    tft.setTextColor(color);
    tft.setTextSize(3);
    char freshStr[8];
    snprintf(freshStr, sizeof(freshStr), "%d%%", fresh);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(freshStr, 232, 16);
    
    // ---- 分隔线 ----
    tft.drawFastHLine(8, 46, 224, COLOR_DIM);
    
    // ---- 传感器数据 (4列) ----
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(1);
    
    // 第一行
    tft.setTextColor(COLOR_DIM);
    tft.drawString("ODOR", 8, 54);
    tft.drawString("HCHO", 62, 54);
    tft.drawString("CO", 124, 54);
    tft.drawString("VOC", 170, 54);
    
    tft.setTextColor(COLOR_TEXT);
    char buf[12];
    snprintf(buf, 8, "%.1f", dispState.odor); tft.drawString(buf, 8, 68);
    snprintf(buf, 8, "%.1f", dispState.hcho); tft.drawString(buf, 62, 68);
    snprintf(buf, 8, "%.2f", dispState.co);   tft.drawString(buf, 124, 68);
    snprintf(buf, 8, "%.1f", dispState.voc);   tft.drawString(buf, 170, 68);
    
    // 第二行: CO2, Temp, Humidity
    tft.setTextColor(COLOR_DIM);
    tft.drawString("CO2", 8, 88);
    tft.drawString("TEMP", 80, 88);
    tft.drawString("HUM", 160, 88);
    
    tft.setTextColor(COLOR_TEXT);
    snprintf(buf, 8, "%u", dispState.co2); tft.drawString(buf, 8, 102);
    snprintf(buf, 8, "%.1fC", dispState.temp); tft.drawString(buf, 80, 102);
    snprintf(buf, 8, "%.0f%%", dispState.humidity); tft.drawString(buf, 160, 102);
    
    // ---- 分隔线 ----
    tft.drawFastHLine(8, 120, 224, COLOR_DIM);
    
    // ---- 置信度条 ----
    tft.setTextColor(COLOR_DIM);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("CONFIDENCE", 8, 128);
    
    // 进度条
    int barX = 8, barY = 144, barW = 224, barH = 8;
    tft.drawRect(barX, barY, barW, barH, COLOR_DIM);
    int fillW = (int)(dispState.confidence * (barW - 2));
    tft.fillRect(barX + 1, barY + 1, fillW, barH - 2, color);
    
    char confStr[8];
    snprintf(confStr, sizeof(confStr), "%.0f%%", dispState.confidence * 100);
    tft.setTextColor(COLOR_TEXT);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(confStr, 232, 128);
    
    // ---- UV状态 ----
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COLOR_DIM);
    tft.drawString("UV STATUS", 8, 160);
    
    if (dispState.uvOn) {
        tft.setTextColor(COLOR_UV);
        tft.setTextSize(2);
        tft.drawString("ON", 8, 176);
        tft.setTextSize(1);
        char uvStr[16];
        snprintf(uvStr, sizeof(uvStr), "%lus left", dispState.uvRemain);
        tft.drawString(uvStr, 40, 180);
    } else {
        tft.setTextColor(COLOR_DIM);
        tft.setTextSize(2);
        tft.drawString("OFF", 8, 176);
    }
    
    // 自动模式标记
    tft.setTextSize(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(dispState.uvAuto ? COLOR_GOOD : COLOR_DIM);
    tft.drawString(dispState.uvAuto ? "AUTO" : "MANUAL", 232, 164);
    
    // ---- BLE状态 (底部) ----
    tft.setTextDatum(BL_DATUM);
    tft.setTextSize(1);
    if (dispState.bleConnected) {
        tft.setTextColor(COLOR_GOOD);
        tft.drawString("BLE: Connected", 8, 276);
    } else {
        tft.setTextColor(COLOR_BAD);
        tft.drawString("BLE: Disconnected", 8, 276);
    }
    
    // 右下角: QR入口提示
    tft.setTextDatum(BR_DATUM);
    tft.setTextColor(COLOR_DIM);
    tft.drawString("'qr show'", 232, 276);
}

// ==================== 新鲜度→颜色映射 ====================
int mapFreshnessToColor(int freshness) {
    if (freshness >= 70) return COLOR_GOOD;
    if (freshness >= 40) return COLOR_WARN;
    return COLOR_BAD;
}

// ==================== 更新显示状态 (线程安全) ====================
void updateDisplayState(const SensorData& data, int predClass, float conf, int freshness) {
    if (xSemaphoreTake(dispMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    
    // 自动切换到运行模式
    if (dispState.mode == DISP_MODE_PAIRING && deviceConnected) {
        dispState.mode = DISP_MODE_RUNNING;
    }
    
    // 更新数据
    const char* name = (predClass >= 0 && predClass < numClasses) ? classNames[predClass] : "?";
    strncpy(dispState.predClass, name, sizeof(dispState.predClass) - 1);
    dispState.confidence = conf;
    dispState.freshness  = freshness;
    dispState.odor  = data.odor_ppm;
    dispState.hcho  = data.hcho_ppm;
    dispState.co    = data.co_ppm;
    dispState.voc   = data.voc_ppm;
    dispState.co2   = data.co2_ppm;
    dispState.temp  = data.env_temp;
    dispState.humidity = data.humidity;
    dispState.uvOn  = uvRelayOn;
    dispState.uvAuto = uvAutoMode;
    dispState.bleConnected = deviceConnected;
    
    if (uvRelayOn && millis() > uvStartTime) {
        unsigned long elapsed = millis() - uvStartTime;
        dispState.uvRemain = (elapsed < uvOnDuration) ? (uvOnDuration - elapsed) / 1000 : 0;
    } else {
        dispState.uvRemain = 0;
    }
    
    dispState.dirty = true;
    xSemaphoreGive(dispMutex);
}

// ==================== 切换显示模式 ====================
void setDisplayMode(DisplayMode mode) {
    if (xSemaphoreTake(dispMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    dispState.mode = mode;
    dispState.dirty = true;
    xSemaphoreGive(dispMutex);
    Serial.printf("[DISP] Mode set to %d\n", mode);
}

// ==================== 查询当前模式 ====================
DisplayMode getDisplayMode() {
    return dispState.mode;
}

#endif // DISPLAY_MANAGER_H
