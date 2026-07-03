/*
 * BME680 Temperature & Humidity Sensor Driver
 * Based on Waveshare official example
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bme680.h"

static const char *TAG = "BME680";

#define BME680_I2C_ADDR       0x77  // 默认地址 (ADDR悬空或接高)
#define BME680_CHIP_ID        0x61

// BME68X 寄存器
#define BME68X_REG_CHIP_ID      0xD0
#define BME68X_REG_SOFT_RESET   0xE0
#define BME68X_REG_STATUS       0xF3
#define BME68X_REG_CTRL_HUM     0x72
#define BME68X_REG_CTRL_MEAS    0x74
#define BME68X_REG_CONFIG       0x75
#define BME68X_REG_CTRL_GAS_0   0x70
#define BME68X_REG_CTRL_GAS_1    0x71
#define BME68X_REG_HEAT_PROF     0x64
#define BME68X_REG_GAS_WAIT      0x64
#define BME68X_REG_FIELD0        0x1D

// 操作模式
#define BME68X_FORCED_MODE      0x01

typedef struct {
    i2c_master_dev_handle_t dev;
    bool initialized;
} bme680_handle_t;

static bme680_handle_t s_bme680 = {0};

static esp_err_t write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_bme680.dev, buf, 2, pdMS_TO_TICKS(100));
}

static esp_err_t read_reg(uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_transmit_receive(s_bme680.dev, &reg, 1, data, len, pdMS_TO_TICKS(100));
}

esp_err_t bme680_init(i2c_master_bus_handle_t i2c_bus) {
    if (s_bme680.initialized) {
        return ESP_OK;
    }

    memset(&s_bme680, 0, sizeof(s_bme680));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BME680_I2C_ADDR,
        .scl_speed_hz = 400000,
    };

    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_bme680.dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Add device failed");
        return ret;
    }

    // Check chip ID
    uint8_t chip_id;
    ret = read_reg(BME68X_REG_CHIP_ID, &chip_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Chip ID read failed");
        return ret;
    }
    
    if (chip_id != BME680_CHIP_ID) {
        ESP_LOGW(TAG, "Chip ID: 0x%02X (expected 0x%02X)", chip_id, BME680_CHIP_ID);
    }
    ESP_LOGI(TAG, "BME68X found, chip ID: 0x%02X", chip_id);

    // Soft reset
    ret = write_reg(BME68X_REG_SOFT_RESET, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 设置 TPH (参考官方 setTPH)
    // osrs_h = 1 (humidity oversampling x1)
    ret = write_reg(BME68X_REG_CTRL_HUM, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));

    // osrs_t = 1, osrs_p = 1, mode = forced
    // 0b 01(T_osrs) 01(P_osrs) 01(mode)
    ret = write_reg(BME68X_REG_CTRL_MEAS, 0x25);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 设置燃气加热 (参考官方 setHeaterProf)
    // nb_conv = 0, run_gas = 1
    ret = write_reg(BME68X_REG_CTRL_GAS_1, 0x10);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 设置加热时间和温度
    // heat_time = 100ms (0x64)
    ret = write_reg(BME68X_REG_GAS_WAIT, 0x64);
    
    s_bme680.initialized = true;
    ESP_LOGI(TAG, "Initialized successfully");
    return ESP_OK;
}

esp_err_t bme680_read(float *temperature, float *humidity, float *pressure) {
    if (!s_bme680.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // 触发强制模式测量
    esp_err_t ret = write_reg(BME68X_REG_CTRL_MEAS, 0x25);
    if (ret != ESP_OK) return ret;

    // 等待测量完成 (官方代码: delay(500 + getMeasDur()/200))
    // BME68X 测量时间大约 60-100ms
    vTaskDelay(pdMS_TO_TICKS(100));

    // 读取字段数据 (17字节)
    uint8_t data[17];
    ret = read_reg(BME68X_REG_FIELD0, data, 17);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read failed");
        return ret;
    }

    ESP_LOGI(TAG, "Field: %02X %02X %02X %02X %02X %02X %02X %02X",
             data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);

    // 检查新数据标志 (bit 7 of data[0])
    if (!(data[0] & 0x80)) {
        ESP_LOGW(TAG, "No new data");
        return ESP_FAIL;
    }

    // 解析数据 (参考官方 datasheet)
    // data[0] = pressure[19:12]
    // data[1] = pressure[11:4]
    // data[2] = pressure[3:0] | temperature[19:16]
    // data[3] = temperature[15:8]
    // data[4] = temperature[7:0]
    // data[5] = temperature[3:0] | humidity[11:8]
    // data[6] = humidity[7:0]
    
    uint32_t adc_P = ((uint32_t)data[0] << 12) | ((uint32_t)data[1] << 4) | ((uint32_t)data[2] >> 4);
    uint32_t adc_T = ((uint32_t)(data[2] & 0x0F) << 16) | ((uint32_t)data[3] << 8) | ((uint32_t)data[4]);
    uint16_t adc_H = ((uint16_t)(data[5] & 0x0F) << 8) | ((uint16_t)data[6]);

    ESP_LOGI(TAG, "ADC: P=%lu, T=%lu, H=%u", adc_P, adc_T, adc_H);

    // 计算温度 (BME68X datasheet)
    // T = (adc_T * 0.0019073486328125) - 25
    *temperature = ((float)adc_T * 0.0019073486328125f) - 25.0f;

    // 计算湿度
    // H = (adc_H / 1024.0) * 100
    *humidity = ((float)adc_H / 1024.0f) * 100.0f;

    // 计算气压 (hPa)
    // P = adc_P * 0.00095367431640625 * 256 = adc_P / 256
    *pressure = ((float)adc_P) / 256.0f;

    return ESP_OK;
}

esp_err_t bme680_deinit(void) {
    if (!s_bme680.initialized) {
        return ESP_OK;
    }
    if (s_bme680.dev) {
        i2c_master_bus_rm_device(s_bme680.dev);
        s_bme680.dev = NULL;
    }
    s_bme680.initialized = false;
    return ESP_OK;
}
