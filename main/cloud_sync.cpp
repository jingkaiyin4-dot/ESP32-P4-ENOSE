#include "cloud_sync.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_wifi.h"
#include "ui.h"
#include "uart_receiver.h"

#include "esp_http_client.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include "model_mgr.h"

static const char *TAG = "CLOUD_SYNC";

extern "C" volatile int g_training_progress;
extern "C" volatile int g_training_cur_epoch;
extern "C" volatile int g_training_tot_epoch;
extern "C" void ui_trigger_csv_list_render(void);
extern "C" void xiaoZhi_tts_broadcast(const char *text);

train_info_t g_train_info = {0};
void *g_train_info_mutex = NULL;
static bool s_download_triggered = false;

#define UPLOAD_URL "http://*.*.*.*/sensor/data" // Censored Server API / 已脱敏服务器API
#define POLL_URL   "http://*.*.*.*/p4/poll?did=***" // Censored Server API / 已脱敏服务器API
#define ACK_URL    "http://*.*.*.*/api/p4/ack?key=***" // Censored Server API / 已脱敏服务器API

static bool s_enabled = false;
static bool s_fast = false;
static volatile bool s_one_shot = false;
static volatile bool s_need_upload_model_info = false;
static volatile bool s_need_upload_model_list = false;
static SemaphoreHandle_t s_sync_sem = NULL;
static TaskHandle_t s_sync_task = NULL;
static lv_timer_t *s_timer = NULL;

static esp_http_client_handle_t s_upload_client = NULL;

static ble_sensor_data_t s_latest_c6_data;
static volatile bool s_new_c6_data_available = false;
static SemaphoreHandle_t s_c6_data_mutex = NULL;

static TaskHandle_t s_train_feed_poll_task_handle = NULL;
static volatile bool s_train_feed_poll_running = false;

/* ── P4 UI aggregate status ─────────────────────────── */
p4_ui_status_t g_p4_ui_status = {0};
void *g_p4_ui_status_mutex = NULL;
#define P4_UI_STATUS_URL "http://*.*.*.*/api/p4/ui/status?p4_id=***" // Censored Server API / 已脱敏服务器API
#define AI_CONTROL_STATUS_URL "http://*.*.*.*/ai-control/status" // Censored Server API / 已脱敏服务器API

/* ── ACK helper ─────────────────────────────────────── */
static void send_ack(const char *cmd_id, bool accept, const char *reason)
{
    cJSON *root = cJSON_CreateObject();
    if (cmd_id) cJSON_AddStringToObject(root, "id", cmd_id);
    cJSON_AddBoolToObject(root, "accept", accept);
    if (reason) cJSON_AddStringToObject(root, "reason", reason);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    esp_http_client_config_t cfg = {
        .url = ACK_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
    free(payload);
}

/* Send command to first paired device via CMD protocol, fallback to bare command */
static void send_cmd_to_paired(const char *command)
{
    char name[32];
    if (uart_receiver_get_first_paired_name(name, sizeof(name))) {
        uart_receiver_send_cmd(name, command);
    } else {
        uart_receiver_send(command);
    }
}

/* ── Command dispatch ────────────────────────────────── */
static void handle_set_uv(cJSON *params, const char *cmd_id)
{
    cJSON *on_item = cJSON_GetObjectItem(params, "on");
    if (!on_item) { send_ack(cmd_id, false, "missing 'on' param"); return; }

    bool on = (on_item->type == cJSON_True) ||
              (on_item->type == cJSON_Number && on_item->valueint != 0);
    send_cmd_to_paired(on ? "uv_on" : "uv_off");
    ui_set_uv(on);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "uv_on", on);
    cJSON *ack = cJSON_CreateObject();
    if (cmd_id) cJSON_AddStringToObject(ack, "id", cmd_id);
    cJSON_AddBoolToObject(ack, "accept", true);
    cJSON_AddItemToObject(ack, "result", result);

    char *payload = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (!payload) return;

    esp_http_client_config_t cfg = { .url = ACK_URL, .method = HTTP_METHOD_POST, .timeout_ms = 5000 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
    free(payload);
}

static void handle_set_fog(cJSON *params, const char *cmd_id)
{
    cJSON *on_item = cJSON_GetObjectItem(params, "on");
    if (!on_item) { send_ack(cmd_id, false, "missing 'on' param"); return; }

    bool on = (on_item->type == cJSON_True) ||
              (on_item->type == cJSON_Number && on_item->valueint != 0);
    send_cmd_to_paired(on ? "fog_on" : "fog_off");
    ui_set_fog(on);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "fog_on", on);
    cJSON *ack = cJSON_CreateObject();
    if (cmd_id) cJSON_AddStringToObject(ack, "id", cmd_id);
    cJSON_AddBoolToObject(ack, "accept", true);
    cJSON_AddItemToObject(ack, "result", result);

    char *payload = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (!payload) return;

    esp_http_client_config_t cfg = { .url = ACK_URL, .method = HTTP_METHOD_POST, .timeout_ms = 5000 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
    free(payload);
}

static void handle_set_fan(cJSON *params, const char *cmd_id)
{
    cJSON *on_item = cJSON_GetObjectItem(params, "on");
    if (!on_item) { send_ack(cmd_id, false, "missing 'on' param"); return; }

    bool on = (on_item->type == cJSON_True) ||
              (on_item->type == cJSON_Number && on_item->valueint != 0);
    send_cmd_to_paired(on ? "fan_on" : "fan_off");
    ui_set_fan(on);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "fan_on", on);
    cJSON *ack = cJSON_CreateObject();
    if (cmd_id) cJSON_AddStringToObject(ack, "id", cmd_id);
    cJSON_AddBoolToObject(ack, "accept", true);
    cJSON_AddItemToObject(ack, "result", result);

    char *payload = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (!payload) return;

    esp_http_client_config_t cfg = { .url = ACK_URL, .method = HTTP_METHOD_POST, .timeout_ms = 5000 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
    free(payload);
}

static void handle_set_lid(cJSON *params, const char *cmd_id)
{
    cJSON *on_item = cJSON_GetObjectItem(params, "on");
    if (!on_item) { send_ack(cmd_id, false, "missing 'on' param"); return; }

    bool on = (on_item->type == cJSON_True) ||
              (on_item->type == cJSON_Number && on_item->valueint != 0);
    send_cmd_to_paired(on ? "lid_on" : "lid_off");
    ui_set_lid(on);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "lid_on", on);
    cJSON *ack = cJSON_CreateObject();
    if (cmd_id) cJSON_AddStringToObject(ack, "id", cmd_id);
    cJSON_AddBoolToObject(ack, "accept", true);
    cJSON_AddItemToObject(ack, "result", result);

    char *payload = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (!payload) return;

    esp_http_client_config_t cfg = { .url = ACK_URL, .method = HTTP_METHOD_POST, .timeout_ms = 5000 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
    free(payload);
}

static void upload_camera_photo(void);

static void handle_ai_control(cJSON *params, const char *cmd_id)
{
    if (!params) { send_ack(cmd_id, false, "missing params"); return; }

    extern bool g_cloud_ai_auto;
    if (!g_cloud_ai_auto) {
        send_ack(cmd_id, false, "ai_auto disabled");
        return;
    }

    extern void uart_receiver_send_cmd(const char *device_name, const char *cmd);
    extern bool uart_receiver_get_first_paired_name(char *name, size_t max_len);

    char device_name[32] = "";
    if (!uart_receiver_get_first_paired_name(device_name, sizeof(device_name)) || strlen(device_name) == 0) {
        strncpy(device_name, "S3_01", sizeof(device_name) - 1);
    }

    cJSON *reason_item = cJSON_GetObjectItem(params, "reason");
    if (reason_item && reason_item->valuestring && strlen(reason_item->valuestring) > 0) {
        extern void ui_show_ai_analysis(const char* result);
        ui_show_ai_analysis(reason_item->valuestring);

        // Voice broadcast logic (HTTP GET stream play)
        static char s_last_spoken_reason[128] = "";
        bool should_speak = false;
        if (cmd_id != NULL) {
            // Explicit command via poll (e.g. test button): always speak!
            should_speak = true;
        } else {
            // Telemetry response: only speak if the reason has changed
            if (strcmp(reason_item->valuestring, s_last_spoken_reason) != 0) {
                should_speak = true;
                strncpy(s_last_spoken_reason, reason_item->valuestring, sizeof(s_last_spoken_reason) - 1);
                s_last_spoken_reason[sizeof(s_last_spoken_reason) - 1] = '\0';
            }
        }
        
        if (should_speak) {
            xiaoZhi_tts_broadcast(reason_item->valuestring);
        }
    }

    cJSON *item;
    bool uv_on = false, fog_on = false, fan_on = false, lid_on = false, photo = false;

    item = cJSON_GetObjectItem(params, "uv");
    if (item && cJSON_IsNumber(item)) {
        uv_on = (item->valueint != 0);
        uart_receiver_send_cmd(device_name, uv_on ? "uv_on" : "uv_off");
        ui_set_uv(uv_on);
    }
    item = cJSON_GetObjectItem(params, "fog");
    if (item && cJSON_IsNumber(item)) {
        fog_on = (item->valueint != 0);
        uart_receiver_send_cmd(device_name, fog_on ? "fog_on" : "fog_off");
        ui_set_fog(fog_on);
    }
    item = cJSON_GetObjectItem(params, "fan");
    if (item && cJSON_IsNumber(item)) {
        fan_on = (item->valueint != 0);
        uart_receiver_send_cmd(device_name, fan_on ? "fan_on" : "fan_off");
        ui_set_fan(fan_on);
    }
    item = cJSON_GetObjectItem(params, "lid");
    if (item && cJSON_IsNumber(item)) {
        lid_on = (item->valueint != 0);
        uart_receiver_send_cmd(device_name, lid_on ? "lid_on" : "lid_off");
        ui_set_lid(lid_on);
    }
    item = cJSON_GetObjectItem(params, "photo");
    if (item && cJSON_IsNumber(item) && item->valueint != 0) {
        photo = true;
        upload_camera_photo();
    }

    if (g_p4_ui_status_mutex &&
        xSemaphoreTake((QueueHandle_t)g_p4_ui_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_p4_ui_status.ai_online = true;
        g_p4_ui_status.ai_trigger_count++;
        g_p4_ui_status.ai_last_trigger_ts = (uint32_t)time(NULL);
        g_p4_ui_status.ai_uv_on = uv_on;
        g_p4_ui_status.ai_fog_on = fog_on;
        g_p4_ui_status.ai_fan_on = fan_on;
        g_p4_ui_status.ai_lid_on = lid_on;
        g_p4_ui_status.ai_take_photo = photo;
        snprintf(g_p4_ui_status.ai_last_reason, sizeof(g_p4_ui_status.ai_last_reason),
            "AI uv=%d fog=%d fan=%d lid=%d photo=%d",
            uv_on, fog_on, fan_on, lid_on, photo);
        xSemaphoreGive((QueueHandle_t)g_p4_ui_status_mutex);
    }

    send_ack(cmd_id, true, NULL);
}

static void handle_alert(cJSON *params)
{
    cJSON *msg = cJSON_GetObjectItem(params, "msg");
    if (msg && msg->valuestring) ui_show_ai_analysis(msg->valuestring);
}

static void handle_reboot(void) { esp_restart(); }

static void handle_report(const char *cmd_id)
{
    int8_t rssi = 0;
    if (wifi_is_connected()) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(result, "rssi", rssi);

    cJSON *ack = cJSON_CreateObject();
    if (cmd_id) cJSON_AddStringToObject(ack, "id", cmd_id);
    cJSON_AddBoolToObject(ack, "accept", true);
    cJSON_AddItemToObject(ack, "result", result);

    char *payload = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (!payload) return;

    esp_http_client_config_t cfg = { .url = ACK_URL, .method = HTTP_METHOD_POST, .timeout_ms = 5000 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
    free(payload);
}

static void recreate_upload_client(void)
{
    if (s_upload_client) {
        esp_http_client_close(s_upload_client);
        esp_http_client_cleanup(s_upload_client);
    }
    esp_http_client_config_t upload_cfg = {
        .url = UPLOAD_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .keep_alive_enable = true,
    };
    s_upload_client = esp_http_client_init(&upload_cfg);
    if (s_upload_client) {
        esp_http_client_set_header(s_upload_client, "X-API-Key", "bigboss");
    }
    ESP_LOGW(TAG, "Upload client re-created");
}

/* ── Poll commands from server ──────────────────────── */
typedef struct {
    char *buf;
    int len;
    int max_len;
} poll_response_ctx_t;

static esp_err_t poll_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        poll_response_ctx_t *ctx = (poll_response_ctx_t *)evt->user_data;
        if (ctx && ctx->buf) {
            int copy_len = evt->data_len;
            if (ctx->len + copy_len < ctx->max_len) {
                memcpy(ctx->buf + ctx->len, evt->data, copy_len);
                ctx->len += copy_len;
                ctx->buf[ctx->len] = '\0';
            } else {
                ESP_LOGE("CLOUD_SYNC", "HTTP Poll Response Buffer Truncated! Max: %d", ctx->max_len);
            }
        }
    }
    return ESP_OK;
}

static void poll_and_execute(void)
{
    if (!wifi_is_connected()) return;
    ESP_LOGD(TAG, "Polling...");

    char buf[2048] = {0};
    poll_response_ctx_t response_ctx = {
        .buf = buf,
        .len = 0,
        .max_len = sizeof(buf) - 1
    };

    /* Use a fresh one-shot client each time — keep-alive GET hangs */
    esp_http_client_config_t cfg = {
        .url = POLL_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .event_handler = poll_http_event_handler,
        .user_data = &response_ctx,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;

    esp_err_t err = esp_http_client_perform(client);
    int status = 0;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGD(TAG, "Poll returned status %d", status);
        }
    } else {
        ESP_LOGW(TAG, "Poll request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);

    int len = response_ctx.len;
    if (status != 200 || len <= 0) { 
        ESP_LOGI(TAG, "Poll OK (empty)"); 
        return; 
    }

    ESP_LOGI(TAG, "Poll response: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;

    cJSON *action = cJSON_GetObjectItem(root, "action");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *id_item = cJSON_GetObjectItem(root, "id");
    const char *cmd_id = (id_item && id_item->valuestring) ? id_item->valuestring : NULL;

    if (action && action->valuestring) {
        ESP_LOGI(TAG, "Received command: action=%s id=%s",
                 action->valuestring, cmd_id ? cmd_id : "null");

        // 模型控制类指令：如果 Receiver 模型未就绪，智能规避并不予下发执行
        bool is_model_cmd = (strcmp(action->valuestring, "model_list") == 0 ||
                             strcmp(action->valuestring, "model_info") == 0 ||
                             strcmp(action->valuestring, "model_switch") == 0 ||
                             strcmp(action->valuestring, "model_delete") == 0 ||
                             strcmp(action->valuestring, "update_model") == 0);
        if (is_model_cmd && !uart_receiver_is_connected()) {
            ESP_LOGW(TAG, "Receiver offline. Ignoring action: %s", action->valuestring);
            send_ack(cmd_id, false, "Receiver offline");
            cJSON_Delete(root);
            return;
        }

        if      (strcmp(action->valuestring, "set_uv") == 0)       handle_set_uv(params, cmd_id);
        else if (strcmp(action->valuestring, "set_fog") == 0)      handle_set_fog(params, cmd_id);
        else if (strcmp(action->valuestring, "set_fan") == 0)      handle_set_fan(params, cmd_id);
        else if (strcmp(action->valuestring, "set_lid") == 0)      handle_set_lid(params, cmd_id);
        else if (strcmp(action->valuestring, "ai_control") == 0)   handle_ai_control(params, cmd_id);
        else if (strcmp(action->valuestring, "alert") == 0)        { handle_alert(params); send_ack(cmd_id, true, NULL); }
        else if (strcmp(action->valuestring, "collect") == 0)      { ui_show_ai_analysis("Data collection triggered remotely"); send_ack(cmd_id, true, NULL); }
        else if (strcmp(action->valuestring, "reboot") == 0)       { send_ack(cmd_id, true, "rebooting"); vTaskDelay(pdMS_TO_TICKS(500)); handle_reboot(); }
        else if (strcmp(action->valuestring, "model_info") == 0)   { send_cmd_to_paired("model_info"); send_ack(cmd_id, true, "Querying model info from S3 Receiver..."); }
        else if (strcmp(action->valuestring, "model_list") == 0)   { send_cmd_to_paired("model_list"); send_ack(cmd_id, true, "Querying model list from S3 Receiver..."); }
        else if (strcmp(action->valuestring, "model_switch") == 0) {
            cJSON *idx = cJSON_GetObjectItem(params, "model_index");
            if (idx) {
                char switch_cmd[64];
                snprintf(switch_cmd, sizeof(switch_cmd), "model load %d", idx->valueint);
                send_cmd_to_paired(switch_cmd);
                send_ack(cmd_id, true, "Command model switch sent successfully");
            } else {
                send_ack(cmd_id, false, "Missing 'model_index' param");
            }
        }
        else if (strcmp(action->valuestring, "model_delete") == 0) {
            cJSON *idx = cJSON_GetObjectItem(params, "model_index");
            if (idx) {
                char del_cmd[64];
                snprintf(del_cmd, sizeof(del_cmd), "model_delete_%d", idx->valueint);
                send_cmd_to_paired(del_cmd);
                send_ack(cmd_id, true, "Command model delete sent successfully");
            } else {
                send_ack(cmd_id, false, "Missing 'model_index' param");
            }
        }
        else if (strcmp(action->valuestring, "uv_dur") == 0) {
            cJSON *dur = cJSON_GetObjectItem(params, "duration");
            if (dur) {
                int seconds = dur->valueint * 60; // 云端下发为分钟，Receiver接收为秒
                char dur_cmd[64];
                snprintf(dur_cmd, sizeof(dur_cmd), "uv_dur_%d", seconds);
                send_cmd_to_paired(dur_cmd);
                send_ack(cmd_id, true, "UV duration synchronized successfully");
            } else {
                send_ack(cmd_id, false, "Missing 'duration' param");
            }
        }
        else if (strcmp(action->valuestring, "fog_dur") == 0) {
            cJSON *dur = cJSON_GetObjectItem(params, "duration");
            if (dur) {
                int seconds = dur->valueint * 60;
                char dur_cmd[64];
                snprintf(dur_cmd, sizeof(dur_cmd), "fog_dur_%d", seconds);
                send_cmd_to_paired(dur_cmd);
                send_ack(cmd_id, true, "Fogger duration synchronized successfully");
            } else {
                send_ack(cmd_id, false, "Missing 'duration' param");
            }
        }
        else if (strcmp(action->valuestring, "fan_dur") == 0) {
            cJSON *dur = cJSON_GetObjectItem(params, "duration");
            if (dur) {
                int seconds = dur->valueint * 60;
                char dur_cmd[64];
                snprintf(dur_cmd, sizeof(dur_cmd), "fan_dur_%d", seconds);
                send_cmd_to_paired(dur_cmd);
                send_ack(cmd_id, true, "Fan duration synchronized successfully");
            } else {
                send_ack(cmd_id, false, "Missing 'duration' param");
            }
        }
        else if (strcmp(action->valuestring, "uv_auto_on") == 0 || strcmp(action->valuestring, "uv_auto_off") == 0) {
            bool auto_on = (strcmp(action->valuestring, "uv_auto_on") == 0);
            send_cmd_to_paired(auto_on ? "uv_auto_on" : "uv_auto_off");
            send_ack(cmd_id, true, "UV auto-mode toggled successfully");
        }
        else if (strcmp(action->valuestring, "fog_auto_on") == 0 || strcmp(action->valuestring, "fog_auto_off") == 0) {
            bool auto_on = (strcmp(action->valuestring, "fog_auto_on") == 0);
            send_cmd_to_paired(auto_on ? "fog_auto_on" : "fog_auto_off");
            send_ack(cmd_id, true, "Fogger auto-mode toggled successfully");
        }
        else if (strcmp(action->valuestring, "fan_auto_on") == 0 || strcmp(action->valuestring, "fan_auto_off") == 0) {
            bool auto_on = (strcmp(action->valuestring, "fan_auto_on") == 0);
            send_cmd_to_paired(auto_on ? "fan_auto_on" : "fan_auto_off");
            send_ack(cmd_id, true, "Fan auto-mode toggled successfully");
        }
        else if (strcmp(action->valuestring, "lid_on") == 0 || strcmp(action->valuestring, "lid_off") == 0) {
            bool lid_on = (strcmp(action->valuestring, "lid_on") == 0);
            send_cmd_to_paired(lid_on ? "lid_on" : "lid_off");
            ui_set_lid(lid_on);
            send_ack(cmd_id, true, "Servo lid toggled successfully");
        }
        else if (strcmp(action->valuestring, "lid_auto_on") == 0 || strcmp(action->valuestring, "lid_auto_off") == 0) {
            bool auto_on = (strcmp(action->valuestring, "lid_auto_on") == 0);
            send_cmd_to_paired(auto_on ? "lid_auto_on" : "lid_auto_off");
            send_ack(cmd_id, true, "Servo lid auto-mode toggled successfully");
        }
        else if (strcmp(action->valuestring, "lid_status") == 0) {
            send_cmd_to_paired("lid_status");
            send_ack(cmd_id, true, "Querying lid status from S3 Receiver...");
        }
        else if (strcmp(action->valuestring, "update_model") == 0) {
            cJSON *url_item = cJSON_GetObjectItem(params, "model_url");
            if (!url_item) url_item = cJSON_GetObjectItem(root, "model_url");

            cJSON *norm_item = cJSON_GetObjectItem(params, "norm_url");
            if (!norm_item) norm_item = cJSON_GetObjectItem(root, "norm_url");
            const char *norm_url_str = (norm_item && norm_item->valuestring) ? norm_item->valuestring : "";

            cJSON *name_item = cJSON_GetObjectItem(params, "model_name");
            if (!name_item) name_item = cJSON_GetObjectItem(root, "model_name");

            cJSON *ver_item = cJSON_GetObjectItem(params, "model_version");
            if (!ver_item) ver_item = cJSON_GetObjectItem(root, "model_version");

            const char *ver_str = (ver_item && ver_item->valuestring) ? ver_item->valuestring : "1.0.0";
            if (url_item && name_item && url_item->valuestring && name_item->valuestring) {
                send_ack(cmd_id, true, "Triggering OTA download and updates...");
                model_mgr_trigger_update(url_item->valuestring, norm_url_str, name_item->valuestring, ver_str, cmd_id);
            } else {
                send_ack(cmd_id, false, "Missing model updates params");
            }
        }
        else if (strcmp(action->valuestring, "report") == 0)       handle_report(cmd_id);
        else if (strcmp(action->valuestring, "skip_warmup") == 0) { send_cmd_to_paired("skip_warmup"); send_ack(cmd_id, true, "Skipping sensor warmup on S3 Receiver"); }
        else                                                        { ESP_LOGW(TAG, "Unknown action: %s", action->valuestring); send_ack(cmd_id, false, "unknown action"); }
    }
    cJSON_Delete(root);
}

/* ── Training Feed and Response Parsers ──────────────── */
static train_status_t parse_status_string(const char *status_str)
{
    if (!status_str) return TRAIN_STATUS_IDLE;
    if (strcmp(status_str, "queued") == 0) return TRAIN_STATUS_QUEUED;
    if (strcmp(status_str, "training") == 0) return TRAIN_STATUS_TRAINING;
    if (strcmp(status_str, "simulating") == 0) return TRAIN_STATUS_SIMULATING;
    if (strcmp(status_str, "completed") == 0 || strcmp(status_str, "success") == 0) return TRAIN_STATUS_COMPLETED;
    if (strcmp(status_str, "failed") == 0) return TRAIN_STATUS_FAILED;
    if (strcmp(status_str, "cancelled") == 0) return TRAIN_STATUS_CANCELLED;
    if (strcmp(status_str, "timeout") == 0) return TRAIN_STATUS_TIMEOUT;
    return TRAIN_STATUS_IDLE;
}

static float extract_accuracy_from_phase(const char *phase_str)
{
    if (!phase_str) return -1.0f;

    const char *p = strstr(phase_str, "准确");
    if (!p) p = strstr(phase_str, "accuracy");
    if (!p) p = strstr(phase_str, "Accuracy");
    if (!p) p = strstr(phase_str, "acc");
    if (!p) p = strstr(phase_str, "Acc");

    if (p) {
        const char *delim = strchr(p, '=');
        const char *colon = strchr(p, ':');
        const char *start = NULL;
        
        if (delim && colon) {
            start = (delim < colon) ? delim : colon;
        } else if (delim) {
            start = delim;
        } else {
            start = colon;
        }
        
        if (start) {
            start++;
            while (*start == ' ') start++;
            
            float val = (float)atof(start);
            bool is_percent = false;
            const char *pct = strchr(start, '%');
            if (pct && (pct - start < 10)) {
                is_percent = true;
            }
            
            if (val > 1.0f || is_percent) {
                return val / 100.0f;
            } else {
                return val;
            }
        }
    }
    return -1.0f;
}

static void upload_camera_photo(void)
{
    uint8_t *jpeg_buf = NULL;
    uint32_t w = 0, h = 0;
    size_t jpeg_len = 0;
    extern bool ui_get_camera_frame(uint8_t **buf, uint32_t *w, uint32_t *h, size_t *len);
    
    if (!ui_get_camera_frame(&jpeg_buf, &w, &h, &jpeg_len) || !jpeg_buf || jpeg_len == 0) {
        ESP_LOGW("CLOUD_SYNC", "take_photo: Camera frame not available");
        return;
    }

    ESP_LOGI("CLOUD_SYNC", "Uploading camera photo, size: %d bytes...", jpeg_len);

    esp_http_client_config_t cfg = {};
    cfg.url = "http://*.*.*.*/upload"; // Censored Server API / 已脱敏服务器API
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 15000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, (const char *)jpeg_buf, jpeg_len);
        esp_http_client_set_header(client, "Content-Type", "image/jpeg");
        esp_err_t r = esp_http_client_perform(client);
        if (r == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            ESP_LOGI("CLOUD_SYNC", "Photo upload success (status=%d)", status);
        } else {
            ESP_LOGE("CLOUD_SYNC", "Photo upload failed: %s", esp_err_to_name(r));
        }
        esp_http_client_cleanup(client);
    }
}

static void parse_sensor_upload_response(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    extern bool g_cloud_ai_auto;
    if (g_cloud_ai_auto) {
        cJSON *ai_ctrl = cJSON_GetObjectItem(root, "ai_control");
        if (ai_ctrl && !cJSON_IsNull(ai_ctrl)) {
            // Convert ai_control to poll params format and reuse the handler
            cJSON *params = cJSON_CreateObject();
            cJSON *item;
            
            // Forward the reason to handle_ai_control so it displays and speaks it
            item = cJSON_GetObjectItem(ai_ctrl, "reason");
            if (item && cJSON_IsString(item)) {
                cJSON_AddStringToObject(params, "reason", item->valuestring);
            }

            item = cJSON_GetObjectItem(ai_ctrl, "uv_on");
            if (item) cJSON_AddNumberToObject(params, "uv", cJSON_IsTrue(item) ? 1 : 0);
            item = cJSON_GetObjectItem(ai_ctrl, "fog_on");
            if (item) cJSON_AddNumberToObject(params, "fog", cJSON_IsTrue(item) ? 1 : 0);
            item = cJSON_GetObjectItem(ai_ctrl, "fan_on");
            if (item) cJSON_AddNumberToObject(params, "fan", cJSON_IsTrue(item) ? 1 : 0);
            item = cJSON_GetObjectItem(ai_ctrl, "lid_on");
            if (item) cJSON_AddNumberToObject(params, "lid", cJSON_IsTrue(item) ? 1 : 0);
            item = cJSON_GetObjectItem(ai_ctrl, "take_photo");
            if (item) cJSON_AddNumberToObject(params, "photo", cJSON_IsTrue(item) ? 1 : 0);

            handle_ai_control(params, NULL);
            cJSON_Delete(params);
        }
    }

    cJSON *train_obj = cJSON_GetObjectItem(root, "train");
    if (train_obj && !cJSON_IsNull(train_obj)) {
        if (!s_train_feed_poll_running) {
            cJSON *id_item = cJSON_GetObjectItem(train_obj, "id");
            cJSON *name_item = cJSON_GetObjectItem(train_obj, "name");
            cJSON *status_item = cJSON_GetObjectItem(train_obj, "status");
            cJSON *phase_item = cJSON_GetObjectItem(train_obj, "phase");
            cJSON *progress_item = cJSON_GetObjectItem(train_obj, "progress");
            
            cJSON *epoch_item = cJSON_GetObjectItem(train_obj, "current_epoch");
            if (!epoch_item) {
                epoch_item = cJSON_GetObjectItem(train_obj, "epoch");
            }
            cJSON *total_epochs_item = cJSON_GetObjectItem(train_obj, "total_epochs");

            cJSON *accuracy_item = cJSON_GetObjectItem(train_obj, "current_accuracy");
            if (!accuracy_item) {
                accuracy_item = cJSON_GetObjectItem(train_obj, "final_accuracy");
            }
            if (!accuracy_item) {
                accuracy_item = cJSON_GetObjectItem(train_obj, "accuracy");
            }
            cJSON *loss_item = cJSON_GetObjectItem(train_obj, "loss");
            if (!loss_item) {
                loss_item = cJSON_GetObjectItem(train_obj, "final_loss");
            }
            cJSON *error_item = cJSON_GetObjectItem(train_obj, "error");

            train_status_t status = status_item && status_item->valuestring ? parse_status_string(status_item->valuestring) : TRAIN_STATUS_IDLE;
            bool is_finished = (status == TRAIN_STATUS_COMPLETED || 
                                status == TRAIN_STATUS_FAILED || 
                                status == TRAIN_STATUS_CANCELLED || 
                                status == TRAIN_STATUS_TIMEOUT);

            if (g_train_info_mutex && xSemaphoreTake((QueueHandle_t)g_train_info_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                const char *resp_id = (id_item && id_item->valuestring) ? id_item->valuestring : "";
                bool active = true;
                if (is_finished) {
                    if (!g_train_info.active || strcmp(g_train_info.training_id, resp_id) != 0) {
                        active = false;
                    }
                }

                g_train_info.active = active;
                if (active) {
                    if (id_item && id_item->valuestring) {
                        strncpy(g_train_info.training_id, id_item->valuestring, sizeof(g_train_info.training_id) - 1);
                    }
                    if (name_item && name_item->valuestring) {
                        strncpy(g_train_info.model_name, name_item->valuestring, sizeof(g_train_info.model_name) - 1);
                    }
                    g_train_info.status = status;
                    if (phase_item && phase_item->valuestring) {
                        strncpy(g_train_info.phase, phase_item->valuestring, sizeof(g_train_info.phase) - 1);
                    } else {
                        g_train_info.phase[0] = '\0';
                    }

                    int cur_ep = 0;
                    if (epoch_item) {
                        if (cJSON_IsNumber(epoch_item)) cur_ep = epoch_item->valueint;
                        else if (cJSON_IsString(epoch_item)) cur_ep = atoi(epoch_item->valuestring);
                    }
                    g_train_info.epoch = cur_ep;
                    g_training_cur_epoch = cur_ep;

                    int tot_ep = 0;
                    if (total_epochs_item) {
                        if (cJSON_IsNumber(total_epochs_item)) tot_ep = total_epochs_item->valueint;
                        else if (cJSON_IsString(total_epochs_item)) tot_ep = atoi(total_epochs_item->valuestring);
                    }
                    g_train_info.total_epochs = tot_ep;
                    g_training_tot_epoch = tot_ep;

                    int progress = 0;
                    if (progress_item) {
                        if (cJSON_IsNumber(progress_item)) progress = progress_item->valueint;
                        else if (cJSON_IsString(progress_item)) progress = atoi(progress_item->valuestring);
                    }
                    if (tot_ep > 0) {
                        int calc_prog = (cur_ep * 100) / tot_ep;
                        if (calc_prog > progress) {
                            progress = calc_prog;
                        }
                    }
                    if (g_train_info.status == TRAIN_STATUS_COMPLETED) {
                        progress = 100;
                    }
                    if (progress < 0) progress = 0;
                    if (progress > 100) progress = 100;
                    g_train_info.progress = progress;

                    // Also keep legacy g_training_progress updated
                    if (g_train_info.status == TRAIN_STATUS_COMPLETED || g_train_info.status == TRAIN_STATUS_FAILED || g_train_info.status == TRAIN_STATUS_CANCELLED || g_train_info.status == TRAIN_STATUS_TIMEOUT) {
                        g_training_progress = -1;
                    } else {
                        g_training_progress = progress;
                    }

                    float parsed_acc = 0.0f;
                    if (accuracy_item) {
                        if (cJSON_IsNumber(accuracy_item)) parsed_acc = (float)accuracy_item->valuedouble;
                        else if (cJSON_IsString(accuracy_item)) parsed_acc = (float)atof(accuracy_item->valuestring);
                    }
                    if (parsed_acc <= 0.0f && phase_item && phase_item->valuestring) {
                        float ext_acc = extract_accuracy_from_phase(phase_item->valuestring);
                        if (ext_acc >= 0.0f) {
                            parsed_acc = ext_acc;
                        }
                    }
                    g_train_info.accuracy = parsed_acc;

                    if (loss_item) {
                        if (cJSON_IsNumber(loss_item)) g_train_info.loss = (float)loss_item->valuedouble;
                        else if (cJSON_IsString(loss_item)) g_train_info.loss = (float)atof(loss_item->valuestring);
                    } else {
                        g_train_info.loss = 0.0f;
                    }

                    if (error_item && error_item->valuestring) {
                        strncpy(g_train_info.error, error_item->valuestring, sizeof(g_train_info.error) - 1);
                    } else {
                        g_train_info.error[0] = '\0';
                    }

                    g_train_info.ts = (uint32_t)time(NULL);

                    if (g_train_info.status == TRAIN_STATUS_QUEUED ||
                        g_train_info.status == TRAIN_STATUS_TRAINING ||
                        g_train_info.status == TRAIN_STATUS_SIMULATING) {
                        s_download_triggered = false;
                    }

                    // Trigger auto-download if completed
                    if (g_train_info.status == TRAIN_STATUS_COMPLETED && !s_download_triggered) {
                        s_download_triggered = true;
                        cJSON *download_url_item = cJSON_GetObjectItem(train_obj, "download_url");
                        cJSON *norm_url_item = cJSON_GetObjectItem(train_obj, "norm_url");
                        char dl_url[256];
                        char norm_url[256];
                        if (download_url_item && download_url_item->valuestring) {
                            strncpy(dl_url, download_url_item->valuestring, sizeof(dl_url) - 1);
                        } else {
                            snprintf(dl_url, sizeof(dl_url), "/model/download/%s/%s.tflite", g_train_info.model_name, g_train_info.model_name);
                        }
                        dl_url[sizeof(dl_url) - 1] = '\0';

                        if (norm_url_item && norm_url_item->valuestring) {
                            strncpy(norm_url, norm_url_item->valuestring, sizeof(norm_url) - 1);
                        } else {
                            snprintf(norm_url, sizeof(norm_url), "/model/download/%s/%s_norm.json", g_train_info.model_name, g_train_info.model_name);
                        }
                        norm_url[sizeof(norm_url) - 1] = '\0';

                        ESP_LOGI(TAG, "Triggering automatic model download from: %s, Norm: %s", dl_url, norm_url);
                        extern void model_mgr_trigger_update(const char *url, const char *norm_url, const char *name, const char *version, const char *cmd_id);
                        model_mgr_trigger_update(dl_url, norm_url, g_train_info.model_name, "1.0.0", "");
                    }
                } else {
                    g_training_progress = -1;
                    g_training_cur_epoch = 0;
                    g_training_tot_epoch = 0;
                }

                xSemaphoreGive((QueueHandle_t)g_train_info_mutex);
            }
        }
    } else {
        if (!s_train_feed_poll_running) {
            uint32_t now = (uint32_t)time(NULL);
            if (g_train_info.ts == 0 || (now - g_train_info.ts > 10)) {
                if (g_train_info_mutex && xSemaphoreTake((QueueHandle_t)g_train_info_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    g_train_info.active = false;
                    g_training_progress = -1;
                    g_training_cur_epoch = 0;
                    g_training_tot_epoch = 0;
                    xSemaphoreGive((QueueHandle_t)g_train_info_mutex);
                }
            }
        }
    }

    cJSON_Delete(root);
}

static void parse_train_feed_response(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    cJSON *active_item = cJSON_GetObjectItem(root, "active");
    bool active = false;
    if (active_item && !cJSON_IsNull(active_item)) {
        active = cJSON_IsTrue(active_item);
    } else {
        // Fallback: check if training_id is present and not null/empty
        cJSON *id_item = cJSON_GetObjectItem(root, "training_id");
        if (id_item && !cJSON_IsNull(id_item) && id_item->valuestring && strlen(id_item->valuestring) > 0) {
            active = true;
        }
    }

    cJSON *status_item = cJSON_GetObjectItem(root, "status");
    train_status_t status = status_item && status_item->valuestring ? parse_status_string(status_item->valuestring) : TRAIN_STATUS_IDLE;
    bool is_finished = (status == TRAIN_STATUS_COMPLETED || 
                        status == TRAIN_STATUS_FAILED || 
                        status == TRAIN_STATUS_CANCELLED || 
                        status == TRAIN_STATUS_TIMEOUT);

    if (g_train_info_mutex && xSemaphoreTake((QueueHandle_t)g_train_info_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cJSON *id_item = cJSON_GetObjectItem(root, "training_id");
        const char *resp_id = (id_item && id_item->valuestring) ? id_item->valuestring : "";

        if (active && is_finished) {
            // A finished training task should only keep/set active = true if the client
            // was already actively tracking it (i.e. g_train_info.active is true and has the same training_id).
            if (!g_train_info.active || strcmp(g_train_info.training_id, resp_id) != 0) {
                active = false;
            }
        }

        g_train_info.active = active;
        if (active) {
            cJSON *id_item = cJSON_GetObjectItem(root, "training_id");
            cJSON *name_item = cJSON_GetObjectItem(root, "model_name");
            cJSON *status_item = cJSON_GetObjectItem(root, "status");
            cJSON *phase_item = cJSON_GetObjectItem(root, "phase");
            cJSON *progress_item = cJSON_GetObjectItem(root, "progress");
            
            cJSON *epoch_item = cJSON_GetObjectItem(root, "current_epoch");
            if (!epoch_item) {
                epoch_item = cJSON_GetObjectItem(root, "epoch");
            }
            cJSON *total_epochs_item = cJSON_GetObjectItem(root, "total_epochs");
            
            cJSON *accuracy_item = cJSON_GetObjectItem(root, "current_accuracy");
            if (!accuracy_item) {
                accuracy_item = cJSON_GetObjectItem(root, "final_accuracy");
            }
            if (!accuracy_item) {
                accuracy_item = cJSON_GetObjectItem(root, "accuracy");
            }
            cJSON *loss_item = cJSON_GetObjectItem(root, "loss");
            if (!loss_item) {
                loss_item = cJSON_GetObjectItem(root, "final_loss");
            }
            cJSON *error_item = cJSON_GetObjectItem(root, "error");

            if (id_item && id_item->valuestring) {
                strncpy(g_train_info.training_id, id_item->valuestring, sizeof(g_train_info.training_id) - 1);
            }
            if (name_item && name_item->valuestring) {
                strncpy(g_train_info.model_name, name_item->valuestring, sizeof(g_train_info.model_name) - 1);
            }
            if (status_item && status_item->valuestring) {
                g_train_info.status = parse_status_string(status_item->valuestring);
            }
            if (phase_item && phase_item->valuestring) {
                strncpy(g_train_info.phase, phase_item->valuestring, sizeof(g_train_info.phase) - 1);
            } else {
                g_train_info.phase[0] = '\0';
            }

            int cur_ep = 0;
            if (epoch_item) {
                if (cJSON_IsNumber(epoch_item)) cur_ep = epoch_item->valueint;
                else if (cJSON_IsString(epoch_item)) cur_ep = atoi(epoch_item->valuestring);
            }
            g_train_info.epoch = cur_ep;
            g_training_cur_epoch = cur_ep;

            int tot_ep = 0;
            if (total_epochs_item) {
                if (cJSON_IsNumber(total_epochs_item)) tot_ep = total_epochs_item->valueint;
                else if (cJSON_IsString(total_epochs_item)) tot_ep = atoi(total_epochs_item->valuestring);
            }
            g_train_info.total_epochs = tot_ep;
            g_training_tot_epoch = tot_ep;

            int progress = 0;
            if (progress_item) {
                if (cJSON_IsNumber(progress_item)) progress = progress_item->valueint;
                else if (cJSON_IsString(progress_item)) progress = atoi(progress_item->valuestring);
            }
            if (tot_ep > 0) {
                int calc_prog = (cur_ep * 100) / tot_ep;
                if (calc_prog > progress) {
                    progress = calc_prog;
                }
            }
            if (g_train_info.status == TRAIN_STATUS_COMPLETED) {
                progress = 100;
            }
            if (progress < 0) progress = 0;
            if (progress > 100) progress = 100;
            g_train_info.progress = progress;

            // Also keep legacy g_training_progress updated
            if (g_train_info.status == TRAIN_STATUS_COMPLETED || g_train_info.status == TRAIN_STATUS_FAILED || g_train_info.status == TRAIN_STATUS_CANCELLED || g_train_info.status == TRAIN_STATUS_TIMEOUT) {
                g_training_progress = -1;
            } else {
                g_training_progress = progress;
            }

            float parsed_acc = 0.0f;
            if (accuracy_item) {
                if (cJSON_IsNumber(accuracy_item)) parsed_acc = (float)accuracy_item->valuedouble;
                else if (cJSON_IsString(accuracy_item)) parsed_acc = (float)atof(accuracy_item->valuestring);
            }
            if (parsed_acc <= 0.0f && phase_item && phase_item->valuestring) {
                float ext_acc = extract_accuracy_from_phase(phase_item->valuestring);
                if (ext_acc >= 0.0f) {
                    parsed_acc = ext_acc;
                }
            }
            g_train_info.accuracy = parsed_acc;

            if (loss_item) {
                if (cJSON_IsNumber(loss_item)) g_train_info.loss = (float)loss_item->valuedouble;
                else if (cJSON_IsString(loss_item)) g_train_info.loss = (float)atof(loss_item->valuestring);
            } else {
                g_train_info.loss = 0.0f;
            }

            if (error_item && error_item->valuestring) {
                strncpy(g_train_info.error, error_item->valuestring, sizeof(g_train_info.error) - 1);
            } else {
                g_train_info.error[0] = '\0';
            }

            if (g_train_info.status == TRAIN_STATUS_QUEUED ||
                g_train_info.status == TRAIN_STATUS_TRAINING ||
                g_train_info.status == TRAIN_STATUS_SIMULATING) {
                s_download_triggered = false;
            }

            // Trigger auto-download if completed
            if (g_train_info.status == TRAIN_STATUS_COMPLETED && !s_download_triggered) {
                s_download_triggered = true;
                cJSON *download_url_item = cJSON_GetObjectItem(root, "download_url");
                cJSON *norm_url_item = cJSON_GetObjectItem(root, "norm_url");
                char dl_url[256];
                char norm_url[256];
                if (download_url_item && download_url_item->valuestring) {
                    strncpy(dl_url, download_url_item->valuestring, sizeof(dl_url) - 1);
                } else {
                    snprintf(dl_url, sizeof(dl_url), "/model/download/%s/%s.tflite", g_train_info.model_name, g_train_info.model_name);
                }
                dl_url[sizeof(dl_url) - 1] = '\0';

                if (norm_url_item && norm_url_item->valuestring) {
                    strncpy(norm_url, norm_url_item->valuestring, sizeof(norm_url) - 1);
                } else {
                    snprintf(norm_url, sizeof(norm_url), "/model/download/%s/%s_norm.json", g_train_info.model_name, g_train_info.model_name);
                }
                norm_url[sizeof(norm_url) - 1] = '\0';

                ESP_LOGI(TAG, "Triggering automatic model download from: %s, Norm: %s", dl_url, norm_url);
                extern void model_mgr_trigger_update(const char *url, const char *norm_url, const char *name, const char *version, const char *cmd_id);
                model_mgr_trigger_update(dl_url, norm_url, g_train_info.model_name, "1.0.0", "");
            }
        } else {
            g_training_progress = -1;
            g_training_cur_epoch = 0;
            g_training_tot_epoch = 0;
        }

        g_train_info.ts = (uint32_t)time(NULL);
        xSemaphoreGive((QueueHandle_t)g_train_info_mutex);
    }

    cJSON_Delete(root);
}

static void train_feed_poll_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Train feed poll task started");
    s_train_feed_poll_running = true;

    while (s_train_feed_poll_running) {
        if (!wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        char *rx_buf = (char *)heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
        if (!rx_buf) {
            ESP_LOGE(TAG, "Failed to allocate 32KB rx buffer for cloud sync!");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        rx_buf[0] = '\0';
        poll_response_ctx_t response_ctx = {
            .buf = rx_buf,
            .len = 0,
            .max_len = 32767
        };

        char url_buf[256];
        char active_id[64] = "";
        if (g_train_info_mutex && xSemaphoreTake((QueueHandle_t)g_train_info_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (strlen(g_train_info.training_id) > 0) {
                strncpy(active_id, g_train_info.training_id, sizeof(active_id) - 1);
            }
            xSemaphoreGive((QueueHandle_t)g_train_info_mutex);
        }

        if (strlen(active_id) > 0) {
            snprintf(url_buf, sizeof(url_buf), "http://*.*.*.*/p4/train/status/%s", active_id); // Censored Server API / 已脱敏服务器API
        } else {
            strncpy(url_buf, "http://*.*.*.*/api/p4/train/feed", sizeof(url_buf) - 1); // Censored Server API / 已脱敏服务器API
            url_buf[sizeof(url_buf) - 1] = '\0';
        }

        esp_http_client_config_t cfg = {};
        cfg.url = url_buf;
        cfg.method = HTTP_METHOD_GET;
        cfg.timeout_ms = 3000;
        cfg.event_handler = poll_http_event_handler;
        cfg.user_data = &response_ctx;
        cfg.keep_alive_enable = false;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client) {
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                int status = esp_http_client_get_status_code(client);
                if (status == 200) {
                    parse_train_feed_response(rx_buf);
                } else {
                    ESP_LOGW(TAG, "Train feed poll status: %d", status);
                }
            } else {
                ESP_LOGE(TAG, "Train feed poll failed: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
        }

        free(rx_buf);
        vTaskDelay(pdMS_TO_TICKS(5000)); // 5s poll
    }

    ESP_LOGI(TAG, "Train feed poll task exiting");
    s_train_feed_poll_task_handle = NULL;
    vTaskDelete(NULL);
}

void cloud_sync_start_train_feed_poll(void)
{
    if (s_train_feed_poll_task_handle != NULL) return;
    s_train_feed_poll_running = true;
    xTaskCreate(train_feed_poll_task, "train_feed_poll", 8192, NULL, 4, &s_train_feed_poll_task_handle);
}

void cloud_sync_stop_train_feed_poll(void)
{
    if (s_train_feed_poll_task_handle == NULL) return;
    s_train_feed_poll_running = false;
}

/* ── Upload sensor data ─────────────────────────────── */
static void upload_sensor(const ui_sensor_data_t *n)
{
    if (!wifi_is_connected()) return;
    if (!s_upload_client) recreate_upload_client();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "t", n->temp);
    cJSON_AddNumberToObject(root, "hu", n->hum);
    cJSON_AddNumberToObject(root, "co2", n->co2);
    cJSON_AddNumberToObject(root, "o", n->odor);
    cJSON_AddNumberToObject(root, "h", n->hcho);
    cJSON_AddNumberToObject(root, "c", n->co);
    cJSON_AddNumberToObject(root, "v", n->voc);
    cJSON_AddStringToObject(root, "cls", n->sensor_class);
    cJSON_AddNumberToObject(root, "conf", n->conf);
    cJSON_AddNumberToObject(root, "fr", n->fresh);
    cJSON_AddBoolToObject(root, "uv_on", n->uv);
    cJSON_AddBoolToObject(root, "fog_on", n->fog);
    cJSON_AddBoolToObject(root, "fan_on", n->fan);
    cJSON_AddBoolToObject(root, "lid_on", n->lid);
    extern bool g_cloud_ai_auto;
    cJSON_AddBoolToObject(root, "ai_auto", g_cloud_ai_auto);
    cJSON_AddStringToObject(root, "did", "p4-01");

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    if (!wifi_is_connected()) { free(payload); return; }
    esp_http_client_set_post_field(s_upload_client, payload, strlen(payload));
    esp_http_client_set_header(s_upload_client, "Content-Type", "application/json");
    ESP_LOGI(TAG, "Starting HTTP POST to %s...", UPLOAD_URL);
    esp_err_t err = esp_http_client_perform(s_upload_client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(s_upload_client);
        char rx_buf[1024];
        int read_len = esp_http_client_read_response(s_upload_client, rx_buf, sizeof(rx_buf) - 1);
        if (read_len >= 0) {
            rx_buf[read_len] = '\0';
            ESP_LOGI(TAG, "Uploaded OK (status=%d), resp: %s", status, rx_buf);
            if (status == 200) {
                parse_sensor_upload_response(rx_buf);
            }
        }
    } else {
        ESP_LOGW(TAG, "Upload failed: %s", esp_err_to_name(err));
        recreate_upload_client();
    }
    free(payload);
}

/* ── Async Model Info Upload ───────────────────────── */
static void upload_model_info(void)
{
    if (!wifi_is_connected()) return;
    ESP_LOGI(TAG, "Starting async HTTP POST model info to cloud...");

    cJSON *data_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(data_obj, "name", g_receiver_model_name);
    cJSON_AddStringToObject(data_obj, "version", g_receiver_model_version);

    cJSON *classes_arr = cJSON_CreateArray();
    if (strlen(g_receiver_model_classes) > 0) {
        char *cls_dup = strdup(g_receiver_model_classes);
        if (cls_dup) {
            char *token = strtok(cls_dup, ",");
            while (token) {
                cJSON_AddItemToArray(classes_arr, cJSON_CreateString(token));
                token = strtok(NULL, ",");
            }
            free(cls_dup);
        }
    }
    cJSON_AddItemToObject(data_obj, "classes", classes_arr);
    cJSON_AddNumberToObject(data_obj, "size", g_receiver_model_size);
    cJSON_AddNumberToObject(data_obj, "accuracy", 0.95);

    cJSON *submit_root = cJSON_CreateObject();
    cJSON_AddStringToObject(submit_root, "type", "model_info");
    cJSON_AddStringToObject(submit_root, "did", "p4-01");
    cJSON_AddItemToObject(submit_root, "data", data_obj);

    char *payload = cJSON_PrintUnformatted(submit_root);
    cJSON_Delete(submit_root);
    if (!payload) return;

    esp_http_client_config_t cfg = {
        .url = "http://*.*.*.*/api/p4/submit?key=***", // Censored Server API / 已脱敏服务器API
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_err_t r = esp_http_client_perform(client);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "Successfully submitted model info asynchronously (name: %s)", g_receiver_model_name);
        } else {
            ESP_LOGW(TAG, "Failed to submit model info asynchronously: %s", esp_err_to_name(r));
        }
        esp_http_client_cleanup(client);
    }
    free(payload);
}

/* ── Async Model List Upload ───────────────────────── */
static void upload_model_list(void)
{
    if (!wifi_is_connected()) return;
    ESP_LOGI(TAG, "Starting async HTTP POST model list to cloud...");

    int model_count = uart_receiver_get_model_count();
    int active_idx = uart_receiver_get_active_index();

    cJSON *submit_root = cJSON_CreateObject();
    cJSON_AddStringToObject(submit_root, "type", "model_list");
    cJSON_AddStringToObject(submit_root, "did", "p4-01");

    cJSON *data_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(data_obj, "active_index", active_idx);

    cJSON *models_arr = cJSON_CreateArray();
    for (int i = 0; i < model_count; i++) {
        const model_item_t *m = uart_receiver_get_model(i);
        if (!m) continue;
        cJSON *m_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(m_obj, "name", m->name);
        cJSON_AddNumberToObject(m_obj, "size", m->size);
        cJSON_AddBoolToObject(m_obj, "active", m->active);

        cJSON *cls_arr = cJSON_CreateArray();
        if (strlen(m->classes) > 0) {
            char *cls_dup = strdup(m->classes);
            if (cls_dup) {
                char *token = strtok(cls_dup, ",");
                while (token) {
                    cJSON_AddItemToArray(cls_arr, cJSON_CreateString(token));
                    token = strtok(NULL, ",");
                }
                free(cls_dup);
            }
        }
        cJSON_AddItemToObject(m_obj, "classes", cls_arr);
        cJSON_AddItemToArray(models_arr, m_obj);
    }
    cJSON_AddItemToObject(data_obj, "models", models_arr);
    cJSON_AddItemToObject(submit_root, "data", data_obj);

    char *payload = cJSON_PrintUnformatted(submit_root);
    cJSON_Delete(submit_root);
    if (!payload) return;

    esp_http_client_config_t cfg = {
        .url = "http://*.*.*.*/api/p4/submit?key=***", // Censored Server API / 已脱敏服务器API
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_err_t r = esp_http_client_perform(client);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "Successfully submitted model list asynchronously");
        } else {
            ESP_LOGW(TAG, "Failed to submit model list asynchronously: %s", esp_err_to_name(r));
        }
        esp_http_client_cleanup(client);
    }
    free(payload);
}

static void upload_device_status(void)
{
    if (!wifi_is_connected()) return;
    ESP_LOGI(TAG, "Uploading device status to cloud...");

    cJSON *submit_root = cJSON_CreateObject();
    cJSON_AddStringToObject(submit_root, "type", "status");
    cJSON_AddStringToObject(submit_root, "did", "p4-01");

    cJSON *data_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(data_obj, "sensor_ok", uart_receiver_is_connected());
    cJSON_AddBoolToObject(data_obj, "camera_ok", ui_is_camera_available());
    cJSON_AddStringToObject(data_obj, "model_name", g_receiver_model_name);
    cJSON_AddNumberToObject(data_obj, "free_heap", esp_get_free_heap_size());

    cJSON_AddItemToObject(submit_root, "data", data_obj);

    char *payload = cJSON_PrintUnformatted(submit_root);
    cJSON_Delete(submit_root);
    if (!payload) return;

    esp_http_client_config_t cfg = {
        .url = "http://*.*.*.*/api/p4/submit?key=***", // Censored Server API / 已脱敏服务器API
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_err_t r = esp_http_client_perform(client);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "Successfully submitted device status");
        } else {
            ESP_LOGW(TAG, "Failed to submit device status: %s", esp_err_to_name(r));
        }
        esp_http_client_cleanup(client);
    }
    free(payload);
}

void cloud_sync_trigger_model_info_upload(void)
{
    s_need_upload_model_info = true;
    if (s_sync_sem) {
        xSemaphoreGive(s_sync_sem);
    }
}

void cloud_sync_trigger_model_list_upload(void)
{
    s_need_upload_model_list = true;
    if (s_sync_sem) {
        xSemaphoreGive(s_sync_sem);
    }
}

/* ── P4 UI aggregate status fetch ───────────────────── */
static void p4_ui_status_set_local_inference(void)
{
    /* Fallback when server endpoint is unavailable: infer from local state. */
    if (!g_p4_ui_status_mutex) return;
    if (xSemaphoreTake((QueueHandle_t)g_p4_ui_status_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    g_p4_ui_status.cloud_online = wifi_is_connected();
    g_p4_ui_status.ai_online = false;

    if (!wifi_is_connected()) {
        strncpy(g_p4_ui_status.ai_state, "offline", sizeof(g_p4_ui_status.ai_state) - 1);
        strncpy(g_p4_ui_status.ai_state_text, "云端离线", sizeof(g_p4_ui_status.ai_state_text) - 1);
        strncpy(g_p4_ui_status.last_action, "正在尝试重新连接云端", sizeof(g_p4_ui_status.last_action) - 1);
    } else if (g_training_progress >= 0) {
        strncpy(g_p4_ui_status.ai_state, "training_model", sizeof(g_p4_ui_status.ai_state) - 1);
        strncpy(g_p4_ui_status.ai_state_text, "AI 训练模型中", sizeof(g_p4_ui_status.ai_state_text) - 1);
        g_p4_ui_status.ai_progress = g_training_progress;
        snprintf(g_p4_ui_status.last_action, sizeof(g_p4_ui_status.last_action),
                 "模型训练进度 %d%%", g_training_progress);
    } else if (s_enabled) {
        strncpy(g_p4_ui_status.ai_state, "collecting_data", sizeof(g_p4_ui_status.ai_state) - 1);
        strncpy(g_p4_ui_status.ai_state_text, "AI 收集数据中", sizeof(g_p4_ui_status.ai_state_text) - 1);
        strncpy(g_p4_ui_status.last_action, "正在汇总传感器与新鲜度数据", sizeof(g_p4_ui_status.last_action) - 1);
    } else {
        strncpy(g_p4_ui_status.ai_state, "idle", sizeof(g_p4_ui_status.ai_state) - 1);
        strncpy(g_p4_ui_status.ai_state_text, "AI 待命中", sizeof(g_p4_ui_status.ai_state_text) - 1);
        strncpy(g_p4_ui_status.last_action, "后台 AI 自动监控就绪", sizeof(g_p4_ui_status.last_action) - 1);
    }

    g_p4_ui_status.updated_at = (uint32_t)time(NULL);
    xSemaphoreGive((QueueHandle_t)g_p4_ui_status_mutex);
}

void cloud_sync_fetch_p4_ui_status(void)
{
    if (!g_p4_ui_status_mutex) return;

    if (!wifi_is_connected()) {
        p4_ui_status_set_local_inference();
        return;
    }

    char rx_buf[2048];
    rx_buf[0] = '\0';
    poll_response_ctx_t response_ctx = {
        .buf = rx_buf,
        .len = 0,
        .max_len = sizeof(rx_buf) - 1
    };

    esp_http_client_config_t cfg = {};
    cfg.url = P4_UI_STATUS_URL;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 4000;
    cfg.event_handler = poll_http_event_handler;
    cfg.user_data = &response_ctx;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        p4_ui_status_set_local_inference();
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (status != 200 || response_ctx.len <= 0) {
        ESP_LOGD(TAG, "p4/ui/status unavailable (status=%d), using local inference", status);
        p4_ui_status_set_local_inference();
        return;
    }

    cJSON *root = cJSON_Parse(rx_buf);
    if (!root) {
        p4_ui_status_set_local_inference();
        return;
    }

    if (xSemaphoreTake((QueueHandle_t)g_p4_ui_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_p4_ui_status.valid = true;
        cJSON *o;
        o = cJSON_GetObjectItem(root, "cloud_online");
        g_p4_ui_status.cloud_online = o ? cJSON_IsTrue(o) : wifi_is_connected();

        o = cJSON_GetObjectItem(root, "ai_state");
        if (o && o->valuestring) strncpy(g_p4_ui_status.ai_state, o->valuestring, sizeof(g_p4_ui_status.ai_state) - 1);
        o = cJSON_GetObjectItem(root, "ai_state_text");
        if (o && o->valuestring) strncpy(g_p4_ui_status.ai_state_text, o->valuestring, sizeof(g_p4_ui_status.ai_state_text) - 1);
        o = cJSON_GetObjectItem(root, "ai_progress");
        if (o) g_p4_ui_status.ai_progress = o->valueint;
        o = cJSON_GetObjectItem(root, "current_task");
        if (o && o->valuestring) strncpy(g_p4_ui_status.current_task, o->valuestring, sizeof(g_p4_ui_status.current_task) - 1);
        o = cJSON_GetObjectItem(root, "sample_today");
        if (o) g_p4_ui_status.sample_today = o->valueint;
        o = cJSON_GetObjectItem(root, "sample_valid");
        if (o) g_p4_ui_status.sample_valid = o->valueint;
        o = cJSON_GetObjectItem(root, "data_quality_text");
        if (o && o->valuestring) strncpy(g_p4_ui_status.data_quality_text, o->valuestring, sizeof(g_p4_ui_status.data_quality_text) - 1);
        o = cJSON_GetObjectItem(root, "model_status_text");
        if (o && o->valuestring) strncpy(g_p4_ui_status.model_status_text, o->valuestring, sizeof(g_p4_ui_status.model_status_text) - 1);
        o = cJSON_GetObjectItem(root, "model_progress");
        if (o) g_p4_ui_status.model_progress = o->valueint;
        o = cJSON_GetObjectItem(root, "storage_plan_text");
        if (o && o->valuestring) strncpy(g_p4_ui_status.storage_plan_text, o->valuestring, sizeof(g_p4_ui_status.storage_plan_text) - 1);
        o = cJSON_GetObjectItem(root, "latest_plan");
        if (o && o->valuestring) strncpy(g_p4_ui_status.latest_plan, o->valuestring, sizeof(g_p4_ui_status.latest_plan) - 1);
        o = cJSON_GetObjectItem(root, "last_action");
        if (o && o->valuestring) strncpy(g_p4_ui_status.last_action, o->valuestring, sizeof(g_p4_ui_status.last_action) - 1);

        cJSON *ac = cJSON_GetObjectItem(root, "auto_control");
        if (ac && !cJSON_IsNull(ac)) {
            cJSON *online_item = cJSON_GetObjectItem(ac, "online");
            g_p4_ui_status.ai_online = online_item ? cJSON_IsTrue(online_item) : false;

            cJSON *ts_item = cJSON_GetObjectItem(ac, "last_trigger_ts");
            g_p4_ui_status.ai_last_trigger_ts = ts_item ? (uint32_t)ts_item->valuedouble : 0;

            cJSON *reason_item = cJSON_GetObjectItem(ac, "last_reason");
            if (reason_item && reason_item->valuestring) {
                strncpy(g_p4_ui_status.ai_last_reason, reason_item->valuestring, sizeof(g_p4_ui_status.ai_last_reason) - 1);
                g_p4_ui_status.ai_last_reason[sizeof(g_p4_ui_status.ai_last_reason) - 1] = '\0';
            } else {
                g_p4_ui_status.ai_last_reason[0] = '\0';
            }

            cJSON *cnt_item = cJSON_GetObjectItem(ac, "trigger_count");
            g_p4_ui_status.ai_trigger_count = cnt_item ? cnt_item->valueint : 0;
        } else {
            g_p4_ui_status.ai_online = false;
        }

        g_p4_ui_status.updated_at = (uint32_t)time(NULL);
        xSemaphoreGive((QueueHandle_t)g_p4_ui_status_mutex);
        ESP_LOGI(TAG, "p4/ui/status OK: ai=%s (%s)", g_p4_ui_status.ai_state, g_p4_ui_status.ai_state_text);
    }
    cJSON_Delete(root);
}

/* ── AI auto-control per-peripheral status fetch ── */
void cloud_sync_fetch_ai_control_status(void)
{
    if (!g_p4_ui_status_mutex) return;

    if (!wifi_is_connected()) return;

    char rx_buf[2048];
    rx_buf[0] = '\0';
    poll_response_ctx_t response_ctx = {
        .buf = rx_buf,
        .len = 0,
        .max_len = sizeof(rx_buf) - 1
    };

    esp_http_client_config_t cfg = {};
    cfg.url = AI_CONTROL_STATUS_URL;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 4000;
    cfg.event_handler = poll_http_event_handler;
    cfg.user_data = &response_ctx;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (status != 200 || response_ctx.len <= 0) return;

    cJSON *root = cJSON_Parse(rx_buf);
    if (!root) return;

    if (xSemaphoreTake((QueueHandle_t)g_p4_ui_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        cJSON *ac = cJSON_GetObjectItem(root, "auto_control");
        if (ac && !cJSON_IsNull(ac)) {
            cJSON *online_item = cJSON_GetObjectItem(ac, "online");
            g_p4_ui_status.ai_online = online_item ? cJSON_IsTrue(online_item) : false;

            cJSON *ts_item = cJSON_GetObjectItem(ac, "last_trigger_ts");
            g_p4_ui_status.ai_last_trigger_ts = ts_item ? (uint32_t)ts_item->valuedouble : 0;

            cJSON *cnt_item = cJSON_GetObjectItem(ac, "trigger_count");
            g_p4_ui_status.ai_trigger_count = cnt_item ? cnt_item->valueint : 0;

            cJSON *reason_item = cJSON_GetObjectItem(ac, "last_reason");
            if (reason_item && reason_item->valuestring) {
                strncpy(g_p4_ui_status.ai_last_reason, reason_item->valuestring, sizeof(g_p4_ui_status.ai_last_reason) - 1);
                g_p4_ui_status.ai_last_reason[sizeof(g_p4_ui_status.ai_last_reason) - 1] = '\0';
            }

            cJSON *o;
            o = cJSON_GetObjectItem(ac, "last_uv_on");
            g_p4_ui_status.ai_uv_on = o ? cJSON_IsTrue(o) : false;
            o = cJSON_GetObjectItem(ac, "last_fog_on");
            g_p4_ui_status.ai_fog_on = o ? cJSON_IsTrue(o) : false;
            o = cJSON_GetObjectItem(ac, "last_fan_on");
            g_p4_ui_status.ai_fan_on = o ? cJSON_IsTrue(o) : false;
            o = cJSON_GetObjectItem(ac, "last_lid_on");
            g_p4_ui_status.ai_lid_on = o ? cJSON_IsTrue(o) : false;
            o = cJSON_GetObjectItem(ac, "last_take_photo");
            g_p4_ui_status.ai_take_photo = o ? cJSON_IsTrue(o) : false;
        }
        xSemaphoreGive((QueueHandle_t)g_p4_ui_status_mutex);
        ESP_LOGI(TAG, "ai-control/status OK: online=%d trigger=%d uv=%d fog=%d fan=%d lid=%d",
            g_p4_ui_status.ai_online, g_p4_ui_status.ai_trigger_count,
            g_p4_ui_status.ai_uv_on, g_p4_ui_status.ai_fog_on,
            g_p4_ui_status.ai_fan_on, g_p4_ui_status.ai_lid_on);
    }
    cJSON_Delete(root);
}

/* ── One-shot: upload + poll (used by both timer and manual trigger) ── */
static void do_upload_and_poll(void)
{
    if (!wifi_is_connected()) return;

    ui_sensor_data_t n1, n2;
    ui_get_node1_data(&n1);
    ui_get_node2_data(&n2);
    if (n1.valid) upload_sensor(&n1);
    if (n2.valid) upload_sensor(&n2);

    poll_and_execute();
}

/* ── Worker task (dedicated thread, no LVGL blocking) ─ */
static void cloud_sync_task(void *pvParameters)
{
    uint32_t print_cnt = 0;
    static uint64_t last_status_upload_us = 0;
    while (1) {
        xSemaphoreTake(s_sync_sem, portMAX_DELAY);

        if (!wifi_is_connected()) continue;

        // Periodic device status upload (every 30 seconds)
        uint64_t now_us = esp_timer_get_time();
        if (now_us - last_status_upload_us > 30000000ULL) {
            upload_device_status();
            last_status_upload_us = now_us;
        }

        /* Upload real-time C6 data if available and enabled */
        bool has_c6_data = false;
        ble_sensor_data_t c6_data;
        if (s_c6_data_mutex && xSemaphoreTake(s_c6_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_new_c6_data_available) {
                c6_data = s_latest_c6_data;
                s_new_c6_data_available = false;
                has_c6_data = true;
            }
            xSemaphoreGive(s_c6_data_mutex);
        }

        extern bool g_cloud_ai_auto;
        static uint64_t last_realtime_upload_us = 0;
        bool should_upload = (s_enabled || g_cloud_ai_auto);
        if (has_c6_data && should_upload) {
            // 限频保护：每 5 秒最多向云端发送一次最新采集数据，避免高频 UART 包引发大量冗余 HTTP 请求
            if (now_us - last_realtime_upload_us >= 5000000ULL) {
                ui_sensor_data_t n;
                n.temp = c6_data.temp;
                n.hum = c6_data.hum;
                n.co2 = c6_data.co2;
                n.odor = c6_data.odor;
                n.hcho = c6_data.hcho;
                n.co = c6_data.co;
                n.voc = c6_data.voc;
                strncpy(n.sensor_class, c6_data.sensor_class, sizeof(n.sensor_class) - 1);
                n.conf = c6_data.conf;
                n.fresh = c6_data.fresh;
                n.uv = c6_data.uv;
                n.fog = c6_data.fog;
                n.fan = c6_data.fan;
                n.lid = c6_data.lid;
                upload_sensor(&n);
                last_realtime_upload_us = now_us;
            }
        }

        /* One-shot manual trigger */
        if (s_one_shot) {
            s_one_shot = false;
            ui_sensor_data_t n1, n2;
            ui_get_node1_data(&n1);
            ui_get_node2_data(&n2);
            if (n1.valid) upload_sensor(&n1);
            if (n2.valid) upload_sensor(&n2);
        }

        /* Always poll for commands */
        poll_and_execute();

        /* Fetch backend AI optimization status for UI display */
        cloud_sync_fetch_p4_ui_status();

        /* Fetch AI auto-control status with per-peripheral trigger info */
        cloud_sync_fetch_ai_control_status();

        /* Async Model Info Upload */
        if (s_need_upload_model_info) {
            s_need_upload_model_info = false;
            upload_model_info();
        }

        /* Async Model List Upload */
        if (s_need_upload_model_list) {
            s_need_upload_model_list = false;
            upload_model_list();
        }

        // 周期性输出系统调试信息（防止刷屏，每10次轮询输出一次）
        if (++print_cnt >= 10) {
            print_cnt = 0;
            ESP_LOGI(TAG, "Debug Monitor - Free Heap: %" PRIu32 " B, Task Stack HWM: %" PRIu32 " B",
                     esp_get_free_heap_size(),
                     (uint32_t)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

static void cloud_sync_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    xSemaphoreGive(s_sync_sem);
}

/* ── Public API ─────────────────────────────────────── */
void cloud_sync_init(void)
{
    s_enabled = false;
    s_fast = false;

    g_train_info_mutex = xSemaphoreCreateMutex();
    g_p4_ui_status_mutex = xSemaphoreCreateMutex();
    s_c6_data_mutex = xSemaphoreCreateMutex();

    s_sync_sem = xSemaphoreCreateBinary();
    if (!s_sync_sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    // Allocate reusable Keep-Alive client for sensors upload
    esp_http_client_config_t upload_cfg = {
        .url = UPLOAD_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .keep_alive_enable = true,
    };
    s_upload_client = esp_http_client_init(&upload_cfg);

    xTaskCreatePinnedToCore(cloud_sync_task, "cloud_sync", 8192, NULL, 3, &s_sync_task, 1);
    s_timer = lv_timer_create(cloud_sync_timer_cb, 5000, NULL);

    ESP_LOGI(TAG, "Cloud sync initialized (5s interval, Keep-Alive active)");
}

void cloud_sync_set_enabled(bool enabled)
{
    s_enabled = enabled;
    s_fast = enabled;
    if (s_timer) {
        lv_timer_set_period(s_timer, enabled ? 1000 : 5000);
    }
    ESP_LOGI(TAG, "Cloud sync %s (interval=%u)", enabled ? "ENABLED" : "DISABLED",
             enabled ? 1000u : 5000u);
}

bool cloud_sync_is_enabled(void)
{
    return s_enabled;
}

void cloud_sync_trigger_once(void)
{
    ESP_LOGI(TAG, "Manual trigger: one-shot upload + poll");
    s_one_shot = true;
    xSemaphoreGive(s_sync_sem);
}

void cloud_sync_post_c6_data(const ble_sensor_data_t *data)
{
    if (!data) return;
    if (s_c6_data_mutex && xSemaphoreTake(s_c6_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_latest_c6_data = *data;
        s_new_c6_data_available = true;
        xSemaphoreGive(s_c6_data_mutex);
    }
    if (s_sync_sem) {
        xSemaphoreGive(s_sync_sem);
    }
}

static void model_training_request_task(void *pvParameters)
{
    if (!wifi_is_connected()) {
        ui_show_ai_analysis("Training request failed: WiFi is offline.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Triggering remote model training: http://*.*.*.*/p4/train/request"); // Censored Server API / 已脱敏服务器API

    esp_http_client_config_t cfg = {};
    cfg.url = "http://*.*.*.*/p4/train/request?key=***"; // Censored Server API / 已脱敏服务器API
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        const char *payload = "{\"csv_files\":[\"sensor_data.csv\"],\"model_name\":\"p4_custom_model\",\"target_accuracy\":85}";
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");

        esp_err_t r = esp_http_client_perform(client);
        if (r == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            if (status == 200) {
                ESP_LOGI(TAG, "Successfully triggered remote model training.");
                ui_show_ai_analysis("Model training triggered successfully. The server has started the training pipeline.");
            } else {
                ESP_LOGE(TAG, "Remote training trigger returned HTTP status: %d", status);
                char buf[128];
                snprintf(buf, sizeof(buf), "Training trigger failed. Server response code: %d", status);
                ui_show_ai_analysis(buf);
            }
        } else {
            ESP_LOGE(TAG, "Failed to perform HTTP POST for training trigger: %s", esp_err_to_name(r));
            char buf[128];
            snprintf(buf, sizeof(buf), "Training trigger failed. Connection error: %s", esp_err_to_name(r));
            ui_show_ai_analysis(buf);
        }
        esp_http_client_cleanup(client);
    } else {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for model training");
        ui_show_ai_analysis("Training trigger failed: Unable to initialize HTTP client.");
    }

    vTaskDelete(NULL);
}

void cloud_sync_trigger_model_training(void)
{
    // Create background task to execute blocking HTTP request safely
    xTaskCreate(model_training_request_task, "train_request", 4096, NULL, 5, NULL);
}

char g_csv_filenames[MAX_CSV_FILES][128] = {0};
int g_csv_file_samples[MAX_CSV_FILES] = {0};
int g_csv_file_count = 0;
volatile int g_csv_fetch_state = 0;

static void fetch_csv_list_task(void *pvParameters)
{
    g_csv_fetch_state = 1; // fetching
    g_csv_file_count = 0;

    if (!wifi_is_connected()) {
        g_csv_fetch_state = 3; // failed
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Fetching CSV list from server...");

    char *rx_buf = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!rx_buf) {
        g_csv_fetch_state = 3;
        vTaskDelete(NULL);
        return;
    }
    rx_buf[0] = '\0';

    poll_response_ctx_t response_ctx = {
        .buf = rx_buf,
        .len = 0,
        .max_len = 8191
    };

    esp_http_client_config_t cfg = {};
    cfg.url = "http://*.*.*.*/api/ai-train/csv-list?key=***"; // Censored Server API / 已脱敏服务器API
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.event_handler = poll_http_event_handler;
    cfg.user_data = &response_ctx;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(rx_buf);
        g_csv_fetch_state = 3;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            ESP_LOGI(TAG, "Fetched CSV raw JSON len: %d", response_ctx.len);
            cJSON *root = cJSON_Parse(rx_buf);
            if (root && cJSON_IsArray(root)) {
                int size = cJSON_GetArraySize(root);
                int idx = 0;
                for (int i = 0; i < size && idx < MAX_CSV_FILES; i++) {
                    cJSON *item = cJSON_GetArrayItem(root, i);
                    if (item) {
                        cJSON *name = cJSON_GetObjectItem(item, "name");
                        cJSON *samples = cJSON_GetObjectItem(item, "samples");
                        if (name && cJSON_IsString(name)) {
                            strncpy(g_csv_filenames[idx], name->valuestring, sizeof(g_csv_filenames[idx]) - 1);
                            g_csv_filenames[idx][sizeof(g_csv_filenames[idx]) - 1] = '\0';
                            g_csv_file_samples[idx] = samples ? samples->valueint : 0;
                            idx++;
                        }
                    }
                }
                g_csv_file_count = idx;
                g_csv_fetch_state = 2; // success
                ESP_LOGI(TAG, "Successfully fetched %d CSV files", g_csv_file_count);
            } else {
                g_csv_fetch_state = 3;
                ESP_LOGE(TAG, "Failed to parse CSV list JSON array. raw: %s", rx_buf);
            }
            if (root) cJSON_Delete(root);
        } else {
            g_csv_fetch_state = 3;
            ESP_LOGE(TAG, "GET /api/ai-train/csv-list returned status %d", status);
        }
    } else {
        g_csv_fetch_state = 3;
        ESP_LOGE(TAG, "GET /api/ai-train/csv-list failed: %s", esp_err_to_name(err));
    }

    free(rx_buf);
    esp_http_client_cleanup(client);

    // Call LVGL async refresh to update the modal screen
    ui_trigger_csv_list_render();

    vTaskDelete(NULL);
}

void cloud_sync_fetch_csv_list(void)
{
    xTaskCreate(fetch_csv_list_task, "fetch_csv_list", 8192, NULL, 5, NULL);
}

struct remote_train_params {
    char csv_files[MAX_CSV_FILES][128];
    int file_count;
    char model_name[64];
    int target_accuracy;
};

static void remote_training_request_task(void *pvParameters)
{
    struct remote_train_params *params = (struct remote_train_params *)pvParameters;
    if (!params) {
        vTaskDelete(NULL);
        return;
    }

    if (!wifi_is_connected()) {
        ui_show_ai_analysis("Training request failed: WiFi is offline.");
        free(params);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Triggering remote training: http://*.*.*.*/p4/train/request"); // Censored Server API / 已脱敏服务器API

    char rx_buf[1024];
    rx_buf[0] = '\0';
    poll_response_ctx_t response_ctx = {
        .buf = rx_buf,
        .len = 0,
        .max_len = sizeof(rx_buf) - 1
    };

    esp_http_client_config_t cfg = {};
    cfg.url = "http://*.*.*.*/p4/train/request?key=***"; // Censored Server API / 已脱敏服务器API
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 10000;
    cfg.event_handler = poll_http_event_handler;
    cfg.user_data = &response_ctx;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        cJSON *root = cJSON_CreateObject();
        cJSON *csv_array = cJSON_CreateArray();
        for (int i = 0; i < params->file_count; i++) {
            cJSON_AddItemToArray(csv_array, cJSON_CreateString(params->csv_files[i]));
        }
        cJSON_AddItemToObject(root, "csv_files", csv_array);
        cJSON_AddStringToObject(root, "model_name", params->model_name);
        cJSON_AddNumberToObject(root, "target_accuracy", params->target_accuracy);

        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            esp_http_client_set_post_field(client, json_str, strlen(json_str));
            esp_http_client_set_header(client, "Content-Type", "application/json");

            esp_err_t r = esp_http_client_perform(client);
            if (r == ESP_OK) {
                int status = esp_http_client_get_status_code(client);
                if (status == 200 || status == 201) {
                    ESP_LOGI(TAG, "Training request success, response: %s", rx_buf);

                    cJSON *resp_json = cJSON_Parse(rx_buf);
                    cJSON *tid = resp_json ? cJSON_GetObjectItem(resp_json, "training_id") : NULL;
                    if (tid && cJSON_IsString(tid)) {
                        char training_id[64];
                        strncpy(training_id, tid->valuestring, sizeof(training_id) - 1);
                        training_id[sizeof(training_id) - 1] = '\0';

                        cJSON_Delete(resp_json);
                        esp_http_client_cleanup(client);
                        free(params);

                        // Initialize training state immediately
                        s_download_triggered = false;
                        if (g_train_info_mutex && xSemaphoreTake((QueueHandle_t)g_train_info_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                            g_train_info.active = true;
                            g_train_info.status = TRAIN_STATUS_QUEUED;
                            g_train_info.progress = 0;
                            g_train_info.epoch = 0;
                            g_train_info.total_epochs = 0;
                            g_train_info.accuracy = 0.0f;
                            g_train_info.loss = 0.0f;
                            g_train_info.error[0] = '\0';
                            strncpy(g_train_info.training_id, training_id, sizeof(g_train_info.training_id) - 1);
                            xSemaphoreGive((QueueHandle_t)g_train_info_mutex);
                        }
                        g_training_progress = 0;
                        g_training_cur_epoch = 0;
                        g_training_tot_epoch = 0;

                        // Start dedicated feed poll task for training status updates
                        cloud_sync_start_train_feed_poll();

                        ui_show_ai_analysis("AI model training requested. Starting monitoring...");
                        vTaskDelete(NULL);
                        return;
                    }
                    if (resp_json) cJSON_Delete(resp_json);
                } else {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Training trigger failed. Server response code: %d", status);
                    ui_show_ai_analysis(buf);
                }
            } else {
                char buf[128];
                snprintf(buf, sizeof(buf), "Training trigger failed. Connection error: %s", esp_err_to_name(r));
                ui_show_ai_analysis(buf);
            }
            cJSON_free(json_str);
        }
        cJSON_Delete(root);
        esp_http_client_cleanup(client);
    } else {
        ui_show_ai_analysis("Training trigger failed: Unable to initialize HTTP client.");
    }

    free(params);
    vTaskDelete(NULL);
}

void cloud_sync_start_remote_training(const char **csv_files, int file_count, const char *model_name, int target_accuracy)
{
    struct remote_train_params *params = (struct remote_train_params *)malloc(sizeof(struct remote_train_params));
    if (!params) return;
    params->file_count = file_count > MAX_CSV_FILES ? MAX_CSV_FILES : file_count;
    for (int i = 0; i < params->file_count; i++) {
        strncpy(params->csv_files[i], csv_files[i], sizeof(params->csv_files[i]) - 1);
        params->csv_files[i][sizeof(params->csv_files[i]) - 1] = '\0';
    }
    strncpy(params->model_name, model_name, sizeof(params->model_name) - 1);
    params->model_name[sizeof(params->model_name) - 1] = '\0';
    params->target_accuracy = target_accuracy;

    xTaskCreate(remote_training_request_task, "remote_train_req", 8192, params, 5, NULL);
}

void cloud_sync_clear_training_state(void)
{
    s_download_triggered = false;
    cloud_sync_stop_train_feed_poll();
    if (g_train_info_mutex && xSemaphoreTake((QueueHandle_t)g_train_info_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_train_info.active = false;
        g_train_info.training_id[0] = '\0';
        g_train_info.status = TRAIN_STATUS_IDLE;
        g_train_info.progress = 0;
        g_train_info.epoch = 0;
        g_train_info.total_epochs = 0;
        g_train_info.accuracy = 0.0f;
        g_train_info.loss = 0.0f;
        g_train_info.error[0] = '\0';
        xSemaphoreGive((QueueHandle_t)g_train_info_mutex);
    }
    g_training_progress = -1;
    g_training_cur_epoch = 0;
    g_training_tot_epoch = 0;
}


