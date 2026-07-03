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
    float temp;
    float hum;
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
    bool fog;
    bool fog_auto;
    int fog_remain;
    int fog_dur;
    bool fan;
    bool fan_auto;
    int fan_remain;
    int fan_dur;
    bool lid;
    bool lid_auto;
    int lid_remain;
    int lid_dur;
    bool mr; // model_ready 状态

} ble_sensor_data_t;
#endif

extern bool g_receiver_model_ready;
extern char g_receiver_model_name[64];
extern char g_receiver_model_version[32];
extern char g_receiver_model_classes[256];
extern int g_receiver_model_size;

#define MAX_MODELS 10
typedef struct {
    char name[64];
    int size;
    bool active;
    char classes[256];
} model_item_t;

typedef void (*ble_sensor_data_cb_t)(const ble_sensor_data_t *data);
typedef void (*warmup_cb_t)(int remaining_seconds);

int uart_receiver_get_model_count(void);
const model_item_t *uart_receiver_get_model(int idx);
int uart_receiver_get_active_index(void);

void uart_receiver_init(void);
void uart_receiver_register_cb(ble_sensor_data_cb_t cb);
void uart_receiver_register_warmup_cb(warmup_cb_t cb);
int uart_receiver_send(const char *str);
int uart_receiver_get_warmup_remaining(void);
void uart_receiver_set_warmup_remaining(int val);
bool uart_receiver_is_connected(void);

// ESP-NOW Pairing & QR Scanner functions
bool uart_receiver_get_mac_by_name(const char *name, char *out_mac, size_t max_len);
void uart_receiver_set_pending_pairing(const char *name);
bool uart_receiver_get_pending_pairing(char *out_name, size_t max_len);
bool uart_receiver_is_paired(const char *name);
void uart_receiver_pair_device(const char *name, const char *mac);
void uart_receiver_send_cmd(const char *name, const char *command);
bool uart_receiver_get_first_paired_name(char *out, size_t max_len);
void uart_receiver_unpair_device(const char *name);
void uart_receiver_clear_all_pairings(void);
int uart_receiver_get_paired_count(void);
bool uart_receiver_get_paired_name_by_index(int idx, char *out, size_t max_len);
void uart_receiver_print_pairings(void);

#ifdef __cplusplus
}
#endif
