#pragma once

#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _BLE_SENSOR_DATA_T_DEF_
#define _BLE_SENSOR_DATA_T_DEF_
typedef struct {
    char node[32];
    float odor;
    float hcho;
    float co;
    float voc;
    int co2;
    int pred;
    char sensor_class[32];
    float conf;
    int fresh;
    bool uv;
    bool uv_auto;
    int uv_remain;
    int uv_dur;
} ble_sensor_data_t;
#endif

typedef void (*ble_sensor_data_cb_t)(const ble_sensor_data_t *data);

#if CONFIG_BT_NIMBLE_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
void ble_central_init(void);
void ble_central_register_cb(ble_sensor_data_cb_t cb);
void ble_central_stop_scan(void);
void ble_central_start_scan(void);
int ble_central_send_data(const char *data);
#else
static inline void ble_central_init(void) {}
static inline void ble_central_register_cb(ble_sensor_data_cb_t cb) { (void)cb; }
static inline void ble_central_stop_scan(void) {}
static inline void ble_central_start_scan(void) {}
static inline int ble_central_send_data(const char *data) { (void)data; return -1; }
#endif

#ifdef __cplusplus
}
#endif
