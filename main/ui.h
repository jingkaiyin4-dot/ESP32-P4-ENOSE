#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "uart_receiver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
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
    bool fog;
    bool fan;
    bool lid;
    bool mr; // 模型就绪状态
} ui_sensor_data_t;

void ui_init(void);
int ui_get_current_volume(void);
void ui_sync_volume(int volume);
void ui_handle_sensor_data(const ble_sensor_data_t *data);
void ui_show_ai_analysis(const char* result);
bool ui_get_camera_frame(uint8_t **buf, uint32_t *w, uint32_t *h, size_t *len);
bool ui_is_camera_available(void);
void ui_get_node1_data(ui_sensor_data_t *out);
void ui_get_node2_data(ui_sensor_data_t *out);
void ui_set_uv(bool on);
void ui_set_fog(bool on);
void ui_set_fan(bool on);
void ui_set_lid(bool on);
void ui_handle_warmup(int remaining);
void ui_send_c6_command(const char *cmd);
const char* ui_get_ai_result(void);
bool wifi_is_connected(void);
extern bool g_cloud_ai_auto;

void ui_update_xiaozhi_text(const char* text);
void ui_update_qr_pairing_result(const char *name, const char *mac, bool success);
void ui_restore_paired_apps(void);

#ifdef __cplusplus
}
#endif
