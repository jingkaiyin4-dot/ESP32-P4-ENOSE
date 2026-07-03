/**
 * 综合气体监测系统 - S3 发送端 (BLE版)
 * ========================================================
 * 通信方式: BLE → ESP32-P4 (通过板载 C6 协处理器)
 * 广播名: "S3-E-Nose"
 * JSON 格式: {"node":"S3_A","voc":1.7,"co2":520,"eth":0.8}
 *
 * 硬件需求:
 *   - ESP32-S3 DevKit
 *   - ADS1115 (I2C) + MQ 气体传感器 x4
 *   - MH-Z19C (UART) CO2 传感器
 *   - BME680 (SPI) 温湿度
 *
 * Arduino 配置:
 *   Board: ESP32S3 Dev Module
 *   PSRAM: OPI PSRAM
 *   Partition: Default 4MB with spiffs / 16MB Flash
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>

#include <DFRobot_ADS1115.h>
#include <Wire.h>
#include <MHZ19.h>
#include <SPI.h>
#include <bme68xLibrary.h>

// ====== BLE 配置 ======
#define BLE_DEVICE_NAME     "S3-E-Nose"
#define BLE_SVC_UUID        "ebe0ccb0-7a0a-4b0c-8a1a-6ffed7a7caaa"
#define BLE_CHR_DATA_UUID   "ebe0ccb1-7a0a-4b0c-8a1a-6ffed7a7caaa"
#define S3_NODE_NAME        "S3_A"

// ====== 引脚定义 ======
#define I2C_SDA 17
#define I2C_SCL 18
#define MHZ_RX  15
#define MHZ_TX  16
#define BME_MOSI 10
#define BME_MISO 11
#define BME_SCK  12
#define BME_CS   5
#define VCC      3.3f

// ====== 传感器参数 ======
const float RL[4] = {4700.0f, 10000.0f, 4700.0f, 4700.0f};
const float A[4]  = {2.357f, 0.351f, 4.082f, 2.357f};
const float B_const = 0.5f;

// ====== 状态位 ======
#define STATUS_ADS1115_OK  0x01
#define STATUS_MHZ19C_OK   0x02
#define STATUS_BME680_OK   0x04

// ====== BLE 对象 ======
BLEServer        *pServer = NULL;
BLECharacteristic *pDataChar = NULL;
bool deviceConnected = false;

// ====== 传感器对象 ======
DFRobot_ADS1115 ads(&Wire);
HardwareSerial   mhSerial(1);
MHZ19           mhz19;
SPIClass        spi(HSPI);
Bme68x          bme;

// ====== 校准数据 ======
float R0[4] = {0, 0, 0, 0};
bool r0Calibrated[4] = {false, false, false, false};

// ====== BME680 缓存 ======
float lastEnvTemp = 25.0f;
float lastHumidity = 50.0f;
bool bmeValid = false;
unsigned long lastBmeReadTime = 0;
const unsigned long bmeReadInterval = 10000;

uint8_t sensorStatus = 0;
unsigned long lastSampleMs = 0;
const unsigned long sampleIntervalMs = 2000;

// ==================== BLE 回调 ====================
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("[BLE] Device connected");
    }

    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("[BLE] Device disconnected, restarting advertising");
        pServer->startAdvertising();
    }
};

// ==================== 传感器底层 ====================
float voltageToPPM(float Vout_mV, int ch) {
    if (Vout_mV <= 0) return 0;
    float Vout = Vout_mV / 1000.0f;
    if (Vout <= 0.001f) return 0;
    float Rs = (VCC - Vout) / Vout * RL[ch];
    float ratio = Rs / R0[ch];
    if (ratio <= 0) return 0;
    return pow(A[ch] / ratio, 1.0f / B_const);
}

void calibrateR0() {
    Serial.println("[CALIB] R0 calibration (clean air)...");
    for (int i = 0; i < 4; i++) {
        for (int retry = 0; retry < 3; retry++) {
            float Vout_mV = ads.readVoltage(i);
            float Vout = Vout_mV / 1000.0f;
            if (Vout <= 0.001f) {
                delay(500);
                continue;
            }
            float Rs = (VCC - Vout) / Vout * RL[i];
            R0[i] = Rs;
            r0Calibrated[i] = true;
            Serial.printf("       Ch%d: R0=%.2f kOhm\n", i, Rs / 1000.0f);
            break;
        }
    }
}

bool initBME680() {
    spi.begin(BME_SCK, BME_MISO, BME_MOSI, BME_CS);
    bme.begin(BME_CS, spi);
    if (bme.checkStatus() != BME68X_OK) return false;
    bme.setTPH(BME68X_OS_8X, BME68X_OS_NONE, BME68X_OS_2X);
    bme.setFilter(BME68X_FILTER_SIZE_3);
    bme.setHeaterProf(0, 0);
    return true;
}

bool readBME680TempHumid(float &temp, float &hum) {
    bme.setOpMode(BME68X_FORCED_MODE);
    uint32_t dur = bme.getMeasDur();
    delay(dur / 1000);
    if (dur % 1000) delayMicroseconds(dur % 1000);
    if (bme.fetchData()) {
        bme68xData d;
        bme.getData(d);
        temp = d.temperature;
        hum  = d.humidity;
        return true;
    }
    return false;
}

// ==================== BLE 初始化 ====================
void initBLE() {
    BLEDevice::init(BLE_DEVICE_NAME);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService *pService = pServer->createService(BLE_SVC_UUID);

    pDataChar = pService->createCharacteristic(
        BLE_CHR_DATA_UUID,
        BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ
    );
    pDataChar->addDescriptor(new BLE2902());
    pDataChar->setValue("{}");

    pService->start();

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(BLE_SVC_UUID);
    pAdvertising->setDeviceName(BLE_DEVICE_NAME);
    pAdvertising->setScanResponse(true, BLE_DEVICE_NAME);
    pAdvertising->setMinPreferred(0x06);
    BLEDevice::startAdvertising();

    Serial.println("[BLE] Advertising as " BLE_DEVICE_NAME);
    Serial.printf("[BLE] Service UUID: %s\n", BLE_SVC_UUID);
}

// ==================== 发送传感器数据 ====================
void sendSensorData() {
    if (!deviceConnected) return;

    // 读取 ADS1115 (4路 MQ 传感器)
    float raw_ppm[4] = {0, 0, 0, 0};
    if (sensorStatus & STATUS_ADS1115_OK) {
        float volt_mV[4] = {
            ads.readVoltage(0),
            ads.readVoltage(1),
            ads.readVoltage(2),
            ads.readVoltage(3)
        };
        for (int i = 0; i < 4; i++) {
            if (r0Calibrated[i]) {
                raw_ppm[i] = voltageToPPM(volt_mV[i], i);
                if (raw_ppm[i] > 5000) raw_ppm[i] = 5000;
            }
        }
    }

    // 读取 MH-Z19C
    int co2_raw = 0;
    if (sensorStatus & STATUS_MHZ19C_OK) {
        for (int retry = 0; retry < 2; retry++) {
            co2_raw = mhz19.getCO2();
            if (co2_raw > 0) break;
            delay(50);
        }
        if (co2_raw <= 0) co2_raw = 0;
    }

    // 读取 BME680
    float env_temp = lastEnvTemp;
    float humidity = lastHumidity;
    if (sensorStatus & STATUS_BME680_OK) {
        unsigned long now = millis();
        if (now - lastBmeReadTime >= bmeReadInterval) {
            lastBmeReadTime = now;
            float t, h;
            if (readBME680TempHumid(t, h)) {
                env_temp = t;   lastEnvTemp = t;
                humidity = h;   lastHumidity = h;
                bmeValid = true;
            } else if (!bmeValid) {
                env_temp = 25.0f; humidity = 50.0f;
            }
        }
    }

    // VOC = raw_ppm[3], Ethylene(odor) = raw_ppm[0]
    float voc = raw_ppm[3];
    int   co2 = co2_raw;
    float eth = raw_ppm[0];

    Serial.printf("[SENSOR] VOC=%.1f CO2=%d Eth=%.1f T=%.1f H=%.1f\n",
        (double)voc, co2, (double)eth, (double)env_temp, (double)humidity);

    // 构建 JSON (匹配 P4 的 parse_sensor_json)
    char json[128];
    snprintf(json, sizeof(json),
        "{\"node\":\"%s\",\"voc\":%.1f,\"co2\":%d,\"eth\":%.1f}",
        S3_NODE_NAME, (double)voc, co2, (double)eth);

    pDataChar->setValue(json);
    pDataChar->notify();
    Serial.printf("[BLE] Sent: %s\n", json);
}

// ==================== setup ====================
void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    Serial.println("\n===== S3 E-Nose Sender (BLE) =====");
    Serial.printf("Node: %s\n", S3_NODE_NAME);

    // PSRAM
    if (psramInit()) {
        Serial.printf("[PSRAM] Total: %d KB, Free: %d KB\n",
            ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);
    }

    Wire.begin(I2C_SDA, I2C_SCL);

    // ADS1115
    ads.setAddr_ADS1115(ADS1115_IIC_ADDRESS0);
    ads.setGain(eGAIN_TWOTHIRDS);
    ads.setMode(eMODE_SINGLE);
    ads.setRate(eRATE_128);
    ads.setOSMode(eOSMODE_SINGLE);
    ads.init();
    delay(20);
    if (ads.checkADS1115()) {
        sensorStatus |= STATUS_ADS1115_OK;
        Serial.println("[OK] ADS1115");
    } else {
        Serial.println("[FAIL] ADS1115");
    }

    // MH-Z19C
    delay(3000);
    mhSerial.begin(9600, SERIAL_8N1, MHZ_RX, MHZ_TX);
    mhz19.begin(mhSerial);
    for (int i = 0; i < 10; i++) {
        delay(500);
        int co2 = mhz19.getCO2();
        if (co2 > 0) {
            sensorStatus |= STATUS_MHZ19C_OK;
            Serial.printf("[OK] MH-Z19C  CO2=%d ppm\n", co2);
            break;
        }
        Serial.printf("     MH-Z19C retry %d/10\n", i + 1);
    }

    // BME680
    if (initBME680()) {
        sensorStatus |= STATUS_BME680_OK;
        Serial.println("[OK] BME680");
        float t, h;
        if (readBME680TempHumid(t, h)) {
            lastEnvTemp = t;
            lastHumidity = h;
            bmeValid = true;
            lastBmeReadTime = millis();
        }
    } else {
        Serial.println("[FAIL] BME680");
    }

    // R0 校准
    if (sensorStatus & STATUS_ADS1115_OK) {
        calibrateR0();
    }

    // BLE 初始化
    initBLE();

    Serial.println("\n===== Ready =====");
    Serial.printf("Advertise as: %s\n", BLE_DEVICE_NAME);
    Serial.printf("Status: 0x%02X\n", sensorStatus);
}

// ==================== loop ====================
void loop() {
    unsigned long now = millis();
    if (now - lastSampleMs >= sampleIntervalMs) {
        lastSampleMs = now;
        sendSensorData();
    }
    delay(10);
}
