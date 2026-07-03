/*
 * BME680 Temperature & Humidity Sensor Driver
 * Simple I2C driver for ESP32-P4
 */

#ifndef BME680_H
#define BME680_H

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize BME680 sensor
esp_err_t bme680_init(i2c_master_bus_handle_t i2c_bus);

// Read temperature, humidity, and pressure
esp_err_t bme680_read(float *temperature, float *humidity, float *pressure);

// Deinitialize BME680
esp_err_t bme680_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // BME680_H
