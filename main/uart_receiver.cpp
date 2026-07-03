#include "uart_receiver.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "model_mgr.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <sys/stat.h>
#include "sd_card_bg.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ui.h"

static const char *TAG = "UART_RECV";

#define UART_PORT            UART_NUM_1
#define UART_TX_PIN          (GPIO_NUM_32)
#define UART_RX_PIN          (GPIO_NUM_36)
#define UART_BAUD            115200
#define UART_BUF_SIZE        (1024)
#define LINE_BUF_SIZE        (512)

static ble_sensor_data_cb_t s_callback = NULL;
static warmup_cb_t s_warmup_cb = NULL;
static char s_line_buf[LINE_BUF_SIZE];
static size_t s_line_len = 0;
static time_t s_last_heartbeat = 0;
static int s_warmup_remaining = -1; /* -1 = not warming up */
static SemaphoreHandle_t s_uart_tx_mutex = NULL;

// 全局模型就绪状态
bool g_receiver_model_ready = false;
char g_receiver_model_name[64] = "food_freshness";
char g_receiver_model_version[32] = "1.0.0";
char g_receiver_model_classes[256] = "air,banana_fresh,apple_fresh,orange_fresh";
int g_receiver_model_size = 4736;

// 多包模型列表缓存定义
static struct {
    model_item_t models[MAX_MODELS];
    int total;
    int active_idx;
    int current_count;
} s_temp_model_list = {0};

extern "C" {
void cloud_sync_trigger_model_info_upload(void);
void cloud_sync_trigger_model_list_upload(void);
}

int uart_receiver_get_model_count(void)
{
    return s_temp_model_list.total;
}

const model_item_t *uart_receiver_get_model(int idx)
{
    if (idx < 0 || idx >= s_temp_model_list.total || idx >= MAX_MODELS) return NULL;
    return &s_temp_model_list.models[idx];
}

int uart_receiver_get_active_index(void)
{
    return s_temp_model_list.active_idx;
}

// ─── ESP-NOW Device Pairing & MAC Learning structures & storage ───
#define MAX_LEARNED_DEVICES 16
typedef struct {
    char name[32];
    char mac[20];
    uint32_t last_seen;
} learned_device_t;

static learned_device_t s_learned_devices[MAX_LEARNED_DEVICES];
static int s_learned_devices_count = 0;

#define MAX_PAIRED_DEVICES 8
typedef struct {
    char name[32];
    char mac[20];
    bool listen_confirmed; // <-- Track if C6 successfully acknowledged the listen command
} paired_device_t;

static paired_device_t s_paired_devices[MAX_PAIRED_DEVICES];
static int s_paired_count = 0;

static char s_pending_pairing_name[32] = "";

static SemaphoreHandle_t s_pairing_mutex = NULL;
static volatile bool s_pairing_dirty = false;

#define NVS_PAIRING_NAMESPACE "pairing_cfg"

static void flush_pairing_list(void)
{
    if (!s_pairing_dirty) return;
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!s_pairing_dirty) {
            xSemaphoreGive(s_pairing_mutex);
            return;
        }
        s_pairing_dirty = false;

        nvs_handle_t handle;
        esp_err_t err = nvs_open(NVS_PAIRING_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to open NVS namespace %s: %s", NVS_PAIRING_NAMESPACE, esp_err_to_name(err));
            xSemaphoreGive(s_pairing_mutex);
            return;
        }
        nvs_erase_all(handle);

        nvs_set_u8(handle, "count", s_paired_count);
        for (int i = 0; i < s_paired_count; i++) {
            char key_name[16];
            char key_mac[16];
            snprintf(key_name, sizeof(key_name), "name_%d", i);
            snprintf(key_mac, sizeof(key_mac), "mac_%d", i);
            nvs_set_str(handle, key_name, s_paired_devices[i].name);
            nvs_set_str(handle, key_mac, s_paired_devices[i].mac);
        }
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Saved %d paired devices to NVS.", s_paired_count);
        xSemaphoreGive(s_pairing_mutex);
    }
}

static void mark_pairing_dirty(void)
{
    s_pairing_dirty = true;
}

static void load_pairing_list(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_PAIRING_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No NVS pairing configuration found.");
        return;
    }
    uint8_t count = 0;
    nvs_get_u8(handle, "count", &count);
    if (count > MAX_PAIRED_DEVICES) count = MAX_PAIRED_DEVICES;

    s_paired_count = count;
    for (int i = 0; i < s_paired_count; i++) {
        char key_name[16];
        char key_mac[16];
        snprintf(key_name, sizeof(key_name), "name_%d", i);
        snprintf(key_mac, sizeof(key_mac), "mac_%d", i);
        size_t len = sizeof(s_paired_devices[i].name);
        nvs_get_str(handle, key_name, s_paired_devices[i].name, &len);
        len = sizeof(s_paired_devices[i].mac);
        nvs_get_str(handle, key_mac, s_paired_devices[i].mac, &len);
        s_paired_devices[i].listen_confirmed = false;
        ESP_LOGI(TAG, "Loaded paired device: %s @ %s", s_paired_devices[i].name, s_paired_devices[i].mac);
    }
    nvs_close(handle);
}

bool uart_receiver_get_mac_by_name(const char *name, char *out_mac, size_t max_len)
{
    if (!name || !out_mac || max_len == 0) return false;
    bool found = false;
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // 1. Search learned devices (populated by C6 forwarded data)
        for (int i = 0; i < s_learned_devices_count; i++) {
            if (strcmp(s_learned_devices[i].name, name) == 0) {
                strncpy(out_mac, s_learned_devices[i].mac, max_len - 1);
                out_mac[max_len - 1] = '\0';
                found = true;
                break;
            }
        }
        // 2. Fallback: search paired devices (restored from NVS after reboot)
        if (!found) {
            for (int i = 0; i < s_paired_count; i++) {
                if (strcmp(s_paired_devices[i].name, name) == 0) {
                    strncpy(out_mac, s_paired_devices[i].mac, max_len - 1);
                    out_mac[max_len - 1] = '\0';
                    found = true;
                    break;
                }
            }
        }
        xSemaphoreGive(s_pairing_mutex);
    }
    return found;
}

void uart_receiver_set_pending_pairing(const char *name)
{
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (name) {
            strncpy(s_pending_pairing_name, name, sizeof(s_pending_pairing_name) - 1);
            s_pending_pairing_name[sizeof(s_pending_pairing_name) - 1] = '\0';
            ESP_LOGI(TAG, "Set pending pairing device name: %s", s_pending_pairing_name);
        } else {
            s_pending_pairing_name[0] = '\0';
        }
        xSemaphoreGive(s_pairing_mutex);
    }
}

bool uart_receiver_get_pending_pairing(char *out_name, size_t max_len)
{
    if (!out_name || max_len == 0) return false;
    bool has_pending = false;
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strlen(s_pending_pairing_name) > 0) {
            strncpy(out_name, s_pending_pairing_name, max_len - 1);
            out_name[max_len - 1] = '\0';
            has_pending = true;
        }
        xSemaphoreGive(s_pairing_mutex);
    }
    return has_pending;
}

bool uart_receiver_is_paired(const char *name)
{
    if (!name) return false;
    bool found = false;
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < s_paired_count; i++) {
            if (strcmp(s_paired_devices[i].name, name) == 0) {
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_pairing_mutex);
    }
    return found;
}

void uart_receiver_pair_device(const char *name, const char *mac)
{
    if (!name || strlen(name) == 0) return;
    if (!mac) mac = "";
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        /* Cancel all other paired devices so C6 only listens for this one */
        if (s_paired_count > 0) {
            uart_receiver_send("CANCEL ALL");
            ESP_LOGI(TAG, "Sent CANCEL ALL before pairing new device: %s", name);
        }
        s_paired_count = 0;

        /* Add new device */
        strncpy(s_paired_devices[0].name, name, sizeof(s_paired_devices[0].name) - 1);
        s_paired_devices[0].name[sizeof(s_paired_devices[0].name) - 1] = '\0';
        strncpy(s_paired_devices[0].mac, mac, sizeof(s_paired_devices[0].mac) - 1);
        s_paired_devices[0].mac[sizeof(s_paired_devices[0].mac) - 1] = '\0';
        s_paired_devices[0].listen_confirmed = false;
        s_paired_count = 1;
        mark_pairing_dirty();

        // Send LISTEN:<name> (C6 auto-learns MAC from ESP-NOW packets)
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "LISTEN:%s", name);
        uart_receiver_send(cmd);
        ESP_LOGI(TAG, "Device paired: %s @ %s, sent -> %s", name, mac, cmd);

        xSemaphoreGive(s_pairing_mutex);
    }
}

void uart_receiver_send_cmd(const char *name, const char *command)
{
    if (!name || !command) return;

    // If this device is in the paired list but listen is not confirmed,
    // re-send LISTEN so C6 has it ready before we send the CMD.
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < s_paired_count; i++) {
            if (strcmp(s_paired_devices[i].name, name) == 0) {
                if (!s_paired_devices[i].listen_confirmed) {
                    char pre[64];
                    snprintf(pre, sizeof(pre), "LISTEN:%s", name);
                    uart_receiver_send(pre);
                    ESP_LOGI(TAG, "pre-CMD: re-sent LISTEN:%s", name);
                }
                break;
            }
        }
        xSemaphoreGive(s_pairing_mutex);
    }

    char buf[1024];
    snprintf(buf, sizeof(buf), "CMD:%s:%s", name, command);
    uart_receiver_send(buf);
    ESP_LOGI(TAG, "CMD -> %s", buf);
}

bool uart_receiver_get_first_paired_name(char *out, size_t max_len)
{
    if (!out || max_len == 0) return false;
    out[0] = '\0';
    bool ok = false;
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_paired_count > 0 && strlen(s_paired_devices[0].name) > 0) {
            strncpy(out, s_paired_devices[0].name, max_len - 1);
            out[max_len - 1] = '\0';
            ok = true;
        }
        xSemaphoreGive(s_pairing_mutex);
    }
    return ok;
}

void uart_receiver_unpair_device(const char *name)
{
    if (!name || strlen(name) == 0) return;
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int found_idx = -1;
        for (int i = 0; i < s_paired_count; i++) {
            if (strcmp(s_paired_devices[i].name, name) == 0) {
                found_idx = i;
                break;
            }
        }
        if (found_idx >= 0) {
            // Send CANCEL command to C6
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "CANCEL:%s", name);
            uart_receiver_send(cmd);
            ESP_LOGI(TAG, "Sent cancel command for %s: -> %s", name, cmd);

            // Shift list
            for (int i = found_idx; i < s_paired_count - 1; i++) {
                s_paired_devices[i] = s_paired_devices[i + 1];
            }
            s_paired_count--;
            mark_pairing_dirty();
        }
        xSemaphoreGive(s_pairing_mutex);
    }
}

void uart_receiver_clear_all_pairings(void)
{
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_paired_count > 0) {
            uart_receiver_send("CANCEL ALL");
            ESP_LOGI(TAG, "Sent CANCEL ALL to C6");
        }
        s_paired_count = 0;
        mark_pairing_dirty();
        ESP_LOGI(TAG, "Pairing list cleared.");
        xSemaphoreGive(s_pairing_mutex);
    }
}

int uart_receiver_get_paired_count(void)
{
    return s_paired_count;
}

bool uart_receiver_get_paired_name_by_index(int idx, char *out, size_t max_len)
{
    if (!out || max_len == 0 || idx < 0 || idx >= s_paired_count) return false;
    bool ok = false;
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (idx < s_paired_count) {
            strncpy(out, s_paired_devices[idx].name, max_len - 1);
            out[max_len - 1] = '\0';
            ok = true;
        }
        xSemaphoreGive(s_pairing_mutex);
    }
    return ok;
}

void uart_receiver_print_pairings(void)
{
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        printf("--- Paired Devices List (%d/%d) ---\n", s_paired_count, MAX_PAIRED_DEVICES);
        for (int i = 0; i < s_paired_count; i++) {
            printf("[%d] Name: %s | MAC: %s\n", i, s_paired_devices[i].name, s_paired_devices[i].mac);
        }
        printf("------------------------------------\n");
        xSemaphoreGive(s_pairing_mutex);
    }
}

/* Look up paired device name by MAC (set via QR code pairing) */
static bool get_paired_name_by_mac(const char *mac, char *out, size_t max_len)
{
    if (!mac || !out || max_len == 0 || !s_pairing_mutex) return false;
    bool found = false;
    if (xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < s_paired_count; i++) {
            if (strcmp(s_paired_devices[i].mac, mac) == 0) {
                strncpy(out, s_paired_devices[i].name, max_len - 1);
                out[max_len - 1] = '\0';
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_pairing_mutex);
    }
    return found;
}

static void parse_and_notify(const char *line)
{
    ESP_LOGI(TAG, "RAW UART from C6: %s", line);
    
    // 寻找真正的 JSON 起始大括号，过滤掉串口前导乱码/脏字节，提升工业级抗干扰能力
    const char *json_start = strchr(line, '{');
    if (!json_start) {
        ESP_LOGW(TAG, "Invalid JSON from C6 (no start brace): %s", line);
        return;
    }

    cJSON *root = cJSON_Parse(json_start);
    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON from C6: %s", json_start);
        return;
    }

    // Since we successfully parsed a valid JSON from C6, the C6 UART connection is active!
    s_last_heartbeat = time(NULL);

    // ---------------- ESP-NOW MAC Learning & Pending Pairing Check ----------------
    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    cJSON *name_item = cJSON_GetObjectItem(root, "name");
    if (!name_item) name_item = cJSON_GetObjectItem(root, "node");
    if (!name_item) name_item = cJSON_GetObjectItem(root, "learned");
    const char *src_mac = (mac_item && mac_item->valuestring) ? mac_item->valuestring : NULL;

    if (mac_item && mac_item->valuestring && name_item && name_item->valuestring) {
        const char *learned_name = name_item->valuestring;
        const char *learned_mac = mac_item->valuestring;

        if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Update learned list
            int learned_idx = -1;
            for (int i = 0; i < s_learned_devices_count; i++) {
                if (strcmp(s_learned_devices[i].name, learned_name) == 0) {
                    learned_idx = i;
                    break;
                }
            }
            if (learned_idx < 0 && s_learned_devices_count < MAX_LEARNED_DEVICES) {
                learned_idx = s_learned_devices_count++;
            }
            if (learned_idx >= 0) {
                strncpy(s_learned_devices[learned_idx].name, learned_name, sizeof(s_learned_devices[learned_idx].name) - 1);
                s_learned_devices[learned_idx].name[sizeof(s_learned_devices[learned_idx].name) - 1] = '\0';
                strncpy(s_learned_devices[learned_idx].mac, learned_mac, sizeof(s_learned_devices[learned_idx].mac) - 1);
                s_learned_devices[learned_idx].mac[sizeof(s_learned_devices[learned_idx].mac) - 1] = '\0';
                s_learned_devices[learned_idx].last_seen = (uint32_t)time(NULL);
                ESP_LOGI(TAG, "Learned device: %s @ %s", learned_name, learned_mac);
            }

            // Auto-bind MAC to paired device with matching name and empty MAC
            for (int i = 0; i < s_paired_count; i++) {
                if (strcmp(s_paired_devices[i].name, learned_name) == 0 && strlen(s_paired_devices[i].mac) == 0) {
                    strncpy(s_paired_devices[i].mac, learned_mac, sizeof(s_paired_devices[i].mac) - 1);
                    s_paired_devices[i].mac[sizeof(s_paired_devices[i].mac) - 1] = '\0';
                    mark_pairing_dirty();
                    ESP_LOGI(TAG, "Auto-bound MAC %s to paired device %s", learned_mac, learned_name);
                }
            }

            // Check pending pairing
            if (strlen(s_pending_pairing_name) > 0 && strcmp(s_pending_pairing_name, learned_name) == 0) {
                ESP_LOGI(TAG, "Pending pairing match found for device %s! MAC: %s", learned_name, learned_mac);

                // Use shared pair function (releases mutex internally)
                xSemaphoreGive(s_pairing_mutex);
                uart_receiver_pair_device(learned_name, learned_mac);
                xSemaphoreTake(s_pairing_mutex, portMAX_DELAY);

                ui_update_qr_pairing_result(learned_name, learned_mac, true);
                s_pending_pairing_name[0] = '\0'; // Clear pending
            }
            xSemaphoreGive(s_pairing_mutex);
        }
    }

    /* ── C6 LISTEN reply: {"listen":"ok","name":"xxx"} or {"error":"xxx"} ── */
    cJSON *listen_item = cJSON_GetObjectItem(root, "listen");
    if (listen_item && listen_item->valuestring) {
        cJSON *name_item2 = cJSON_GetObjectItem(root, "name");
        const char *dev_name = name_item2 ? name_item2->valuestring : "?";
        cJSON *mac_item2 = cJSON_GetObjectItem(root, "mac");
        const char *dev_mac = (mac_item2 && mac_item2->valuestring) ? mac_item2->valuestring : NULL;

        if (strcmp(listen_item->valuestring, "ok") == 0) {
            ESP_LOGI(TAG, "C6 confirmed LISTEN for %s", dev_name);
            if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                for (int i = 0; i < s_paired_count; i++) {
                    if (strcmp(s_paired_devices[i].name, dev_name) == 0) {
                        s_paired_devices[i].listen_confirmed = true;
                        if (dev_mac && strlen(dev_mac) > 0 && strlen(s_paired_devices[i].mac) == 0) {
                            strncpy(s_paired_devices[i].mac, dev_mac, sizeof(s_paired_devices[i].mac) - 1);
                            s_paired_devices[i].mac[sizeof(s_paired_devices[i].mac) - 1] = '\0';
                            mark_pairing_dirty();
                            ESP_LOGI(TAG, "Updated MAC from listen reply: %s @ %s", dev_name, dev_mac);
                        }
                    }
                }
                xSemaphoreGive(s_pairing_mutex);
            }
            ui_update_qr_pairing_result(dev_name, dev_mac ? dev_mac : "", true);
        } else {
            ESP_LOGW(TAG, "C6 LISTEN failed for %s: %s", dev_name, listen_item->valuestring);
            ui_update_qr_pairing_result(dev_name, NULL, false);
        }
        cJSON_Delete(root);
        return;
    }

    /* ── C6 CANCEL reply: {"cancel":"ok","name":"xxx"} ── */
    cJSON *cancel_item = cJSON_GetObjectItem(root, "cancel");
    if (cancel_item && cancel_item->valuestring) {
        cJSON *name_item3 = cJSON_GetObjectItem(root, "name");
        const char *dev_name = name_item3 ? name_item3->valuestring : "?";
        ESP_LOGI(TAG, "C6 CANCEL reply for %s: %s", dev_name, cancel_item->valuestring);
        cJSON_Delete(root);
        return;
    }

    /* ── C6 cmd reply: {"cmd":"pending/flushed","name":"xxx","to":"yyy"} ── */
    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (cmd_item && cmd_item->valuestring) {
        cJSON *name_item_cmd = cJSON_GetObjectItem(root, "name");
        cJSON *to_item = cJSON_GetObjectItem(root, "to");
        const char *cmd_val = cmd_item->valuestring;
        const char *dev_name = name_item_cmd ? name_item_cmd->valuestring : "?";
        const char *to_val = to_item ? to_item->valuestring : "?";

        if (strcmp(cmd_val, "pending") == 0) {
            ESP_LOGI(TAG, "C6 pending: command '%s' cached for device '%s' (waiting for MAC learning...)", to_val, dev_name);
        } else if (strcmp(cmd_val, "flushed") == 0) {
            ESP_LOGI(TAG, "C6 flushed: cached command '%s' sent to device '%s' (MAC learned!)", to_val, dev_name);
        } else {
            ESP_LOGI(TAG, "C6 command status: %s (name=%s, to=%s)", cmd_val, dev_name, to_val);
        }
        cJSON_Delete(root);
        return;
    }

    /* ── C6 error reply: {"error":"xxx"} ── */
    cJSON *err_item = cJSON_GetObjectItem(root, "error");
    if (err_item && err_item->valuestring) {
        cJSON *name_item4 = cJSON_GetObjectItem(root, "name");
        ESP_LOGW(TAG, "C6 error: %s (name=%s)", err_item->valuestring,
                 name_item4 ? name_item4->valuestring : "?");

        // Self-heal: "not_found" or "not_listened" means C6 no longer has this
        // device in its listen list (e.g. C6 rebooted, or name not registered).
        // Re-send LISTEN so C6 registers it and learns the MAC on next sensor packet.
        if (name_item4 && name_item4->valuestring &&
            (strcmp(err_item->valuestring, "not_found") == 0 ||
             strcmp(err_item->valuestring, "not_listened") == 0))
        {
            const char *lost_name = name_item4->valuestring;
            if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                for (int i = 0; i < s_paired_count; i++) {
                    if (strcmp(s_paired_devices[i].name, lost_name) == 0) {
                        s_paired_devices[i].listen_confirmed = false;
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "LISTEN:%s", lost_name);
                        uart_receiver_send(cmd);
                        ESP_LOGI(TAG, "%s => re-sent LISTEN:%s", err_item->valuestring, lost_name);
                        break;
                    }
                }
                xSemaphoreGive(s_pairing_mutex);
            }
        }

        cJSON_Delete(root);
        return;
    }

    /* Bridge heartbeat: {"bridge":"heartbeat","ble":"up","age":0,"listen":X} */
    cJSON *bridge = cJSON_GetObjectItem(root, "bridge");
    if (bridge && bridge->valuestring && strcmp(bridge->valuestring, "heartbeat") == 0) {
        cJSON *ble_item = cJSON_GetObjectItem(root, "ble");
        cJSON *age_item = cJSON_GetObjectItem(root, "age");
        cJSON *listen_cnt_item = cJSON_GetObjectItem(root, "listen");
        int listen_cnt = listen_cnt_item ? listen_cnt_item->valueint : -1;

        ESP_LOGI(TAG, "Bridge heartbeat (BLE:%s, age:%d, listen_count:%d)",
                 ble_item && ble_item->valuestring ? ble_item->valuestring : "?",
                 age_item ? age_item->valueint : -1,
                 listen_cnt);
        
        s_last_heartbeat = time(NULL);

        // Self-healing: if C6 listen count is less than P4 paired count (e.g. C6 rebooted), re-send LISTEN commands
        if (listen_cnt >= 0 && s_paired_count > 0 && listen_cnt < s_paired_count) {
            ESP_LOGW(TAG, "C6 reports listen_count (%d) < paired_count (%d). Re-sending LISTEN commands...", listen_cnt, s_paired_count);
            if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                for (int i = 0; i < s_paired_count; i++) {
                    s_paired_devices[i].listen_confirmed = false;
                    char cmd[64];
                    snprintf(cmd, sizeof(cmd), "LISTEN:%s", s_paired_devices[i].name);
                    uart_receiver_send(cmd);
                    ESP_LOGI(TAG, "Self-healed LISTEN command -> %s", cmd);
                }
                xSemaphoreGive(s_pairing_mutex);
            }
        }

        cJSON_Delete(root);
        return;
    }

    /* Warmup: {"type":"warmup","remaining":120} */
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (type && type->valuestring) {
        if (strcmp(type->valuestring, "warmup") == 0) {
            cJSON *rem = cJSON_GetObjectItem(root, "remaining");
            s_warmup_remaining = rem ? rem->valueint : 0;
            ESP_LOGI(TAG, "Warmup remaining: %d s", s_warmup_remaining);
            if (s_warmup_cb) s_warmup_cb(s_warmup_remaining);
            cJSON_Delete(root);
            return;
        }
        if (strcmp(type->valuestring, "hb") == 0) {
            ESP_LOGI(TAG, "Heartbeat from C6");
            s_last_heartbeat = time(NULL);
            cJSON_Delete(root);
            return;
        }
        if (strcmp(type->valuestring, "model_list") == 0) {
            cJSON *total = cJSON_GetObjectItem(root, "total");
            cJSON *active = cJSON_GetObjectItem(root, "active");
            s_temp_model_list.total = total ? total->valueint : 0;
            s_temp_model_list.active_idx = active ? active->valueint : 0;
            s_temp_model_list.current_count = 0;
            memset(s_temp_model_list.models, 0, sizeof(s_temp_model_list.models));
            ESP_LOGI(TAG, "Start multi-packet model list: total=%d, active=%d", s_temp_model_list.total, s_temp_model_list.active_idx);
            cJSON_Delete(root);
            return;
        }
        if (strcmp(type->valuestring, "model_detail") == 0) {
            cJSON *idx = cJSON_GetObjectItem(root, "idx");
            cJSON *name = cJSON_GetObjectItem(root, "model");
            cJSON *size = cJSON_GetObjectItem(root, "size");
            cJSON *active = cJSON_GetObjectItem(root, "active");
            cJSON *classes = cJSON_GetObjectItem(root, "classes");
            int n = idx ? idx->valueint : -1;
            if (n >= 0 && n < MAX_MODELS) {
                if (name && name->valuestring) strncpy(s_temp_model_list.models[n].name, name->valuestring, sizeof(s_temp_model_list.models[n].name) - 1);
                s_temp_model_list.models[n].size = size ? size->valueint : 0;
                s_temp_model_list.models[n].active = (active && active->valueint != 0);
                if (classes && classes->valuestring) {
                    strncpy(s_temp_model_list.models[n].classes, classes->valuestring, sizeof(s_temp_model_list.models[n].classes) - 1);
                } else {
                    s_temp_model_list.models[n].classes[0] = '\0';
                }
                s_temp_model_list.current_count++;
                ESP_LOGI(TAG, "Received model detail: idx=%d name=%s size=%d", n, s_temp_model_list.models[n].name, s_temp_model_list.models[n].size);
            }
            cJSON_Delete(root);
            return;
        }
        if (strcmp(type->valuestring, "model_list_end") == 0) {
            ESP_LOGI(TAG, "Received model_list_end. Replying model list_ack to S3 & triggering async cloud list upload.");
            // 1. 回复 Ack 给 C6/S3
            uart_receiver_send("model list_ack");
            
            // 2. 触发异步上传至云端
            cloud_sync_trigger_model_list_upload();

            // 3. 同步刷新当前活跃的模型名字、类标签与大小至全局变量，使屏幕与上报即时呈现
            int active_idx = s_temp_model_list.active_idx;
            if (active_idx >= 0 && active_idx < s_temp_model_list.total && active_idx < MAX_MODELS) {
                const model_item_t *m = &s_temp_model_list.models[active_idx];
                if (strlen(m->name) > 0) {
                    // 过滤掉后缀 .tflite
                    char base_name[64];
                    strncpy(base_name, m->name, sizeof(base_name) - 1);
                    base_name[sizeof(base_name) - 1] = '\0';
                    char *dot = strstr(base_name, ".tflite");
                    if (dot) *dot = '\0';

                    strncpy(g_receiver_model_name, base_name, sizeof(g_receiver_model_name) - 1);
                    g_receiver_model_name[sizeof(g_receiver_model_name) - 1] = '\0';

                    strncpy(g_receiver_model_classes, m->classes, sizeof(g_receiver_model_classes) - 1);
                    g_receiver_model_classes[sizeof(g_receiver_model_classes) - 1] = '\0';

                    g_receiver_model_size = m->size;
                    g_receiver_model_ready = true;

                    // 同步触发模型信息上报，零阻塞
                    cloud_sync_trigger_model_info_upload();
                }
            }

            cJSON_Delete(root);
            return;
        }
        if (strcmp(type->valuestring, "fog_status") == 0) {
            ble_sensor_data_t data;
            memset(&data, 0, sizeof(data));
            strncpy(data.node, "S3_Receiver", sizeof(data.node) - 1);
            strncpy(data.sensor_class, "fog_status", sizeof(data.sensor_class) - 1);

            cJSON *state = cJSON_GetObjectItem(root, "state");
            cJSON *auto_val = cJSON_GetObjectItem(root, "auto");
            cJSON *remain = cJSON_GetObjectItem(root, "remain");

            data.fog = state && (state->valueint != 0);
            data.fog_auto = auto_val && (auto_val->valueint != 0);
            data.fog_remain = remain ? remain->valueint : 0;

            { char pn[32]; if (src_mac && get_paired_name_by_mac(src_mac, pn, sizeof(pn))) strncpy(data.node, pn, sizeof(data.node) - 1); }
            if (s_callback) s_callback(&data);
            cJSON_Delete(root);
            return;
        }
        if (strcmp(type->valuestring, "fan_status") == 0) {
            ble_sensor_data_t data;
            memset(&data, 0, sizeof(data));
            strncpy(data.node, "S3_Receiver", sizeof(data.node) - 1);
            strncpy(data.sensor_class, "fan_status", sizeof(data.sensor_class) - 1);

            cJSON *state = cJSON_GetObjectItem(root, "state");
            cJSON *auto_val = cJSON_GetObjectItem(root, "auto");
            cJSON *remain = cJSON_GetObjectItem(root, "remain");

            data.fan = state && (state->valueint != 0);
            data.fan_auto = auto_val && (auto_val->valueint != 0);
            data.fan_remain = remain ? remain->valueint : 0;

            { char pn[32]; if (src_mac && get_paired_name_by_mac(src_mac, pn, sizeof(pn))) strncpy(data.node, pn, sizeof(data.node) - 1); }
            if (s_callback) s_callback(&data);
            cJSON_Delete(root);
            return;
        }
        if (strcmp(type->valuestring, "lid_status") == 0) {
            ble_sensor_data_t data;
            memset(&data, 0, sizeof(data));
            strncpy(data.node, "S3_Receiver", sizeof(data.node) - 1);
            strncpy(data.sensor_class, "lid_status", sizeof(data.sensor_class) - 1);

            cJSON *state = cJSON_GetObjectItem(root, "state");
            cJSON *auto_val = cJSON_GetObjectItem(root, "auto");
            cJSON *remain = cJSON_GetObjectItem(root, "remain");

            data.lid = state && (state->valueint != 0);
            data.lid_auto = auto_val && (auto_val->valueint != 0);
            data.lid_remain = remain ? remain->valueint : 0;

            { char pn[32]; if (src_mac && get_paired_name_by_mac(src_mac, pn, sizeof(pn))) strncpy(data.node, pn, sizeof(data.node) - 1); }
            if (s_callback) s_callback(&data);
            cJSON_Delete(root);
            return;
        }
        if (strcmp(type->valuestring, "sensor") != 0) {
            cJSON_Delete(root);
            return;
        }
    }

    /* MCP/Model OTA Handshake: {"mt":"chunk_ok",...} or {"mi":1,...} */
    cJSON *mt_item = cJSON_GetObjectItem(root, "mt");
    if (mt_item && mt_item->valuestring && strlen(mt_item->valuestring) > 0) {
        cJSON *mid_item = cJSON_GetObjectItem(root, "mid");
        int mid = mid_item ? mid_item->valueint : 0;
        
        model_mgr_handle_s3_msg(mt_item->valuestring, mid);
        
        cJSON_Delete(root);
        return;
    }

    cJSON *mi_item = cJSON_GetObjectItem(root, "mi");
    if (mi_item) {
        cJSON *mn = cJSON_GetObjectItem(root, "mn");
        cJSON *mv = cJSON_GetObjectItem(root, "mv");
        cJSON *mc = cJSON_GetObjectItem(root, "mc");
        cJSON *ms = cJSON_GetObjectItem(root, "ms");

        // 仅当 mi 不为 0 (代表本帧真实包含模型详情) 且 mn 名字有效时，说明是真实的有效详情包，才予以更新和触发异步上报
        if (mi_item->valueint != 0 && mn && mn->valuestring && strlen(mn->valuestring) > 0) {
            strncpy(g_receiver_model_name, mn->valuestring, sizeof(g_receiver_model_name) - 1);
            if (mv && mv->valuestring) strncpy(g_receiver_model_version, mv->valuestring, sizeof(g_receiver_model_version) - 1);
            if (mc && mc->valuestring) strncpy(g_receiver_model_classes, mc->valuestring, sizeof(g_receiver_model_classes) - 1);
            if (ms) g_receiver_model_size = ms->valueint;

            // 触发异步模型信息上报，零阻塞
            cloud_sync_trigger_model_info_upload();
        }

        // 智能双向分流保护：如果此帧不包含传感器关键测量字段，则为纯模型信息帧，即时拦截退出
        if (!cJSON_GetObjectItem(root, "o") && !cJSON_GetObjectItem(root, "odor")) {
            cJSON_Delete(root);
            return;
        }
    }

    /* Parse as sensor data (abbreviated or full format) */
    ble_sensor_data_t data;
    memset(&data, 0, sizeof(data));

    /* Node name: prefer QR-paired name if MAC is in s_paired_devices */
    {
        char paired_name[32] = "";
        if (src_mac) get_paired_name_by_mac(src_mac, paired_name, sizeof(paired_name));
        if (paired_name[0]) {
            strncpy(data.node, paired_name, sizeof(data.node) - 1);
        } else {
            cJSON *ni = cJSON_GetObjectItem(root, "node");
            if (!ni) ni = cJSON_GetObjectItem(root, "name");
            if (ni && ni->valuestring)
                strncpy(data.node, ni->valuestring, sizeof(data.node) - 1);
            else
                strncpy(data.node, "S3_Receiver", sizeof(data.node) - 1);
        }
    }

    cJSON *item;

    item = cJSON_GetObjectItem(root, "t");
    if (!item) item = cJSON_GetObjectItem(root, "temp");
    if (item) data.temp = (float)item->valuedouble;

    item = cJSON_GetObjectItem(root, "hu");
    if (!item) item = cJSON_GetObjectItem(root, "hum");
    if (item) data.hum = (float)item->valuedouble;

    item = cJSON_GetObjectItem(root, "o");
    if (!item) item = cJSON_GetObjectItem(root, "odor");
    if (item) data.odor = (float)item->valuedouble;

    item = cJSON_GetObjectItem(root, "h");
    if (!item) item = cJSON_GetObjectItem(root, "hcho");
    if (item) data.hcho = (float)item->valuedouble;

    item = cJSON_GetObjectItem(root, "c");
    if (!item) item = cJSON_GetObjectItem(root, "co");
    if (item) data.co = (float)item->valuedouble;

    item = cJSON_GetObjectItem(root, "v");
    if (!item) item = cJSON_GetObjectItem(root, "voc");
    if (item) data.voc = (float)item->valuedouble;

    item = cJSON_GetObjectItem(root, "co2");
    if (item) data.co2 = item->valueint;

    item = cJSON_GetObjectItem(root, "pred");
    if (item) data.pred = item->valueint;

    // Heuristic: Check if the incoming packet contains any actual telemetry fields.
    // If none are present, it is a pure state acknowledgement/status packet.
    bool has_telemetry = (cJSON_GetObjectItem(root, "t") != NULL) ||
                         (cJSON_GetObjectItem(root, "temp") != NULL) ||
                         (cJSON_GetObjectItem(root, "hu") != NULL) ||
                         (cJSON_GetObjectItem(root, "hum") != NULL) ||
                         (cJSON_GetObjectItem(root, "o") != NULL) ||
                         (cJSON_GetObjectItem(root, "odor") != NULL) ||
                         (cJSON_GetObjectItem(root, "h") != NULL) ||
                         (cJSON_GetObjectItem(root, "hcho") != NULL) ||
                         (cJSON_GetObjectItem(root, "c") != NULL) ||
                         (cJSON_GetObjectItem(root, "co") != NULL) ||
                         (cJSON_GetObjectItem(root, "v") != NULL) ||
                         (cJSON_GetObjectItem(root, "voc") != NULL) ||
                         (cJSON_GetObjectItem(root, "co2") != NULL) ||
                         (cJSON_GetObjectItem(root, "pred") != NULL);

    if (!has_telemetry) {
        strncpy(data.sensor_class, "status_only", sizeof(data.sensor_class) - 1);
    } else {
        item = cJSON_GetObjectItem(root, "cls");
        if (!item) item = cJSON_GetObjectItem(root, "sensor_class");
        if (item && item->valuestring) {
            strncpy(data.sensor_class, item->valuestring, sizeof(data.sensor_class) - 1);
        }
    }

    item = cJSON_GetObjectItem(root, "conf");
    if (item) data.conf = (float)item->valuedouble;

    item = cJSON_GetObjectItem(root, "fr");
    if (!item) item = cJSON_GetObjectItem(root, "fresh");
    if (item) data.fresh = item->valueint;

    item = cJSON_GetObjectItem(root, "uv");
    if (item) data.uv = (item->type == cJSON_True) || (item->type == cJSON_Number && item->valueint != 0);

    item = cJSON_GetObjectItem(root, "ua");
    if (!item) item = cJSON_GetObjectItem(root, "uv_auto");
    if (item) data.uv_auto = (item->type == cJSON_True) || (item->type == cJSON_Number && item->valueint != 0);

    item = cJSON_GetObjectItem(root, "ur");
    if (!item) item = cJSON_GetObjectItem(root, "uv_remain");
    if (item) data.uv_remain = item->valueint;

    item = cJSON_GetObjectItem(root, "fo");
    if (!item) item = cJSON_GetObjectItem(root, "fog");
    if (item) data.fog = (item->type == cJSON_True) || (item->type == cJSON_Number && item->valueint != 0);

    item = cJSON_GetObjectItem(root, "fa");
    if (!item) item = cJSON_GetObjectItem(root, "fog_auto");
    if (item) data.fog_auto = (item->type == cJSON_True) || (item->type == cJSON_Number && item->valueint != 0);

    item = cJSON_GetObjectItem(root, "fn");
    if (!item) item = cJSON_GetObjectItem(root, "fan");
    if (item) data.fan = (item->type == cJSON_True) || (item->type == cJSON_Number && item->valueint != 0);

    item = cJSON_GetObjectItem(root, "fl");
    if (!item) item = cJSON_GetObjectItem(root, "fan_auto");
    if (item) data.fan_auto = (item->type == cJSON_True) || (item->type == cJSON_Number && item->valueint != 0);

    item = cJSON_GetObjectItem(root, "lo");
    if (!item) item = cJSON_GetObjectItem(root, "lid");
    if (item) data.lid = (item->type == cJSON_True) || (item->type == cJSON_Number && item->valueint != 0);

    item = cJSON_GetObjectItem(root, "la");
    if (!item) item = cJSON_GetObjectItem(root, "lid_auto");
    if (item) data.lid_auto = (item->type == cJSON_True) || (item->type == cJSON_Number && item->valueint != 0);

    item = cJSON_GetObjectItem(root, "lr");
    if (!item) item = cJSON_GetObjectItem(root, "lid_remain");
    if (item) data.lid_remain = item->valueint;

    item = cJSON_GetObjectItem(root, "ld");
    if (!item) item = cJSON_GetObjectItem(root, "lid_dur");
    if (item) data.lid_dur = item->valueint;

    item = cJSON_GetObjectItem(root, "mr");
    if (!item) item = cJSON_GetObjectItem(root, "model_ready");
    if (item) {
        data.mr = (item->type == cJSON_True) || (item->type == cJSON_Number && item->valueint != 0);
        g_receiver_model_ready = data.mr;
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Sensor data: node=%s t=%.1f hu=%.1f o=%.2f h=%.2f c=%.1f v=%.1f co2=%d cls=%s conf=%.1f fr=%d",
             data.node, data.temp, data.hum, data.odor, data.hcho, data.co, data.voc,
             data.co2, data.sensor_class, data.conf, data.fresh);

    if (s_callback) {
        s_callback(&data);
    }
}

static void uart_rx_task(void *pvParameters)
{
    uint8_t buf[128];
    uint32_t last_flush_check = 0;
    while (1) {
        int len = uart_read_bytes(UART_PORT, buf, sizeof(buf) - 1, pdMS_TO_TICKS(50));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char c = (char)buf[i];
                if (c == '\n' || c == '\r') {
                    if (s_line_len > 0) {
                        s_line_buf[s_line_len] = '\0';
                        parse_and_notify(s_line_buf);
                        s_line_len = 0;
                    }
                } else {
                    if (s_line_len < LINE_BUF_SIZE - 1) {
                        s_line_buf[s_line_len++] = c;
                    }
                }
            }
        }
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - last_flush_check > 5000) {
            last_flush_check = now;
            flush_pairing_list();

            // Periodic LISTEN retry for unconfirmed devices
            if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                for (int i = 0; i < s_paired_count; i++) {
                    if (!s_paired_devices[i].listen_confirmed) {
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "LISTEN:%s", s_paired_devices[i].name);
                        uart_receiver_send(cmd);
                        ESP_LOGI(TAG, "Retrying unconfirmed LISTEN command -> %s", cmd);
                    }
                }
                xSemaphoreGive(s_pairing_mutex);
            }
        }
    }
}

void uart_receiver_sync_local_models(void)
{
    if (!sd_card_bg_is_ready()) {
        esp_err_t err = sd_card_bg_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Local models sync skipped: Failed to mount SD card (%s)", esp_err_to_name(err));
            return;
        }
    }

    int count = uart_receiver_get_model_count();
    ESP_LOGI(TAG, "Syncing local models from SD card... Found %d models", count);

    if (count > 0) {
        const model_item_t *active_model = NULL;
        for (int i = 0; i < count; i++) {
            const model_item_t *m = uart_receiver_get_model(i);
            if (m && m->active) {
                active_model = m;
                break;
            }
        }
        if (!active_model) {
            active_model = uart_receiver_get_model(0);
        }

        if (active_model) {
            char base_name[64] = {0};
            int name_len = strlen(active_model->name);
            if (name_len > 7 && strcmp(active_model->name + name_len - 7, ".tflite") == 0) {
                strncpy(base_name, active_model->name, name_len - 7);
            } else {
                strncpy(base_name, active_model->name, sizeof(base_name) - 1);
            }

            strncpy(g_receiver_model_name, base_name, sizeof(g_receiver_model_name) - 1);
            g_receiver_model_size = active_model->size;
            g_receiver_model_ready = true;

            ESP_LOGI(TAG, "Successfully loaded local active model from SD: name=%s, size=%d B",
                     g_receiver_model_name, g_receiver_model_size);
        }
    }
}

void uart_receiver_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0));

    s_uart_tx_mutex = xSemaphoreCreateMutex();
    s_pairing_mutex = xSemaphoreCreateMutex();
    s_line_len = 0;

    // Load paired list from NVS
    load_pairing_list();

    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "UART receiver initialized (TX:%d, RX:%d, %d baud)",
             UART_TX_PIN, UART_RX_PIN, UART_BAUD);

    // 确保开机挂载物理 SD 卡
    sd_card_bg_init();

    // Restore LISTEN command sequence for all paired devices to C6 bridge on boot (Only send name, let C6 learn MAC dynamically)
    if (s_pairing_mutex && xSemaphoreTake(s_pairing_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < s_paired_count; i++) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "LISTEN:%s", s_paired_devices[i].name);
            uart_receiver_send(cmd);
            ESP_LOGI(TAG, "Restored pairing: sent UART command -> %s", cmd);
        }
        xSemaphoreGive(s_pairing_mutex);
    }

    // 主动向下位机 S3 节点索要模型列表，在串口控制台上即时刷新体现
    char first_paired[32] = "";
    if (uart_receiver_get_first_paired_name(first_paired, sizeof(first_paired)) && strlen(first_paired) > 0) {
        uart_receiver_send_cmd(first_paired, "model list");
    } else {
        uart_receiver_send("model list");
    }
}

void uart_receiver_register_cb(ble_sensor_data_cb_t cb)
{
    s_callback = cb;
}

int uart_receiver_send(const char *json_str)
{
    if (!json_str) return -1;
    if (!s_uart_tx_mutex) return -1;

    xSemaphoreTake(s_uart_tx_mutex, portMAX_DELAY);
    size_t len = strlen(json_str);
    int written = uart_write_bytes(UART_PORT, json_str, len);
    uart_write_bytes(UART_PORT, "\n", 1);
    xSemaphoreGive(s_uart_tx_mutex);

    return written;
}

void uart_receiver_register_warmup_cb(warmup_cb_t cb)
{
    s_warmup_cb = cb;
}

int uart_receiver_get_warmup_remaining(void)
{
    return s_warmup_remaining;
}

void uart_receiver_set_warmup_remaining(int val)
{
    s_warmup_remaining = val;
}

bool uart_receiver_is_connected(void)
{
    if (s_last_heartbeat == 0) return false;
    return (difftime(time(NULL), s_last_heartbeat) < 90.0);
}
