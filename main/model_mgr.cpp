#include "model_mgr.h"
#include "uart_receiver.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "sd_card_bg.h"
#include <sys/stat.h>
#include <sys/unistd.h>

extern "C" void cloud_sync_trigger_model_info_upload(void);

static const char *MM_TAG = "MODEL_MGR";

static ModelUpdateCtx s_model_ctx = {
    .model_name = "",
    .model_version = "",
    .model_url = "",
    .norm_url = "",
    .total_chunks = 0,
    .total_size = 0,
    .current_chunk = 0,
    .file_data = NULL,
    .state = MCS_IDLE,
    .last_action_time = 0,
    .retry_count = 0,
    .cmd_id = ""
};

// MinMaxScaler and class mapping parameters
float g_model_min[16] = {0.0f};
float g_model_max[16] = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f
};
char g_model_classes[8][32] = {0};
int g_model_num_classes = 0;
bool g_model_has_norm = false;

static bool is_valid_tflite(const uint8_t *buffer, size_t size)
{
    if (size < 8) {
        return false;
    }
    // Check for FlatBuffer identifier "TFL3" at offset 4
    if (buffer[4] == 'T' && buffer[5] == 'F' && buffer[6] == 'L' && buffer[7] == '3') {
        return true;
    }
    return false;
}

static void log_invalid_model_header(const uint8_t *buffer, size_t size)
{
    char hex_dump[96] = "";
    char ascii_dump[33] = "";
    size_t dump_len = (size < 32) ? size : 32;
    for (size_t i = 0; i < dump_len; i++) {
        sprintf(hex_dump + (i * 2), "%02X", buffer[i]);
        if (buffer[i] >= 32 && buffer[i] <= 126) {
            ascii_dump[i] = buffer[i];
        } else {
            ascii_dump[i] = '.';
        }
    }
    ascii_dump[dump_len] = '\0';
    ESP_LOGE(MM_TAG, "Invalid model header: hex=%s, ascii=%s", hex_dump, ascii_dump);
}

static void submit_update_result(const char *model_name, const char *status, const char *error_msg)
{
    cJSON *data_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(data_obj, "name", model_name);
    cJSON_AddStringToObject(data_obj, "status", status);
    cJSON_AddStringToObject(data_obj, "error", error_msg ? error_msg : "");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "model_update_result");
    cJSON_AddStringToObject(root, "did", "p4-01");
    cJSON_AddItemToObject(root, "data", data_obj);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    esp_http_client_config_t cfg = {};
    cfg.url = "http://*.*.*.*/api/p4/submit?key=***"; // Censored Server API / 已脱敏服务器API
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 5000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }

    free(payload);
    ESP_LOGI(MM_TAG, "Submitted update result. model=%s, status=%s", model_name, status);
}

static void post_progress(const char* name, int total, int done) {
    static int lastPct = -1;
    int pct = (total > 0) ? (done * 100 / total) : 0;
    if (pct == lastPct) return;   // 不变不报，省流量
    lastPct = pct;

    cJSON *data_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(data_obj, "name", name);
    cJSON_AddNumberToObject(data_obj, "bytes_total", total);
    cJSON_AddNumberToObject(data_obj, "bytes_done", done);
    cJSON_AddNumberToObject(data_obj, "percent", pct);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "model_download_progress");
    cJSON_AddStringToObject(root, "did", "p4-01");
    cJSON_AddItemToObject(root, "data", data_obj);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    esp_http_client_config_t cfg = {};
    cfg.url = "http://*.*.*.*/api/p4/submit?key=***"; // Censored Server API / 已脱敏服务器API
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 3000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
    free(payload);
}

#define DOWNLOAD_GROW_SIZE (16 * 1024)

/* Try up to 3 URL path variants: /api/model/..., /api/p4/model/..., /model/... (direct) */
static int build_download_url(const char *url, char *full_url, size_t max_len, int variant)
{
    const char *domain = "http://*.*.*.*"; // Censored Server API / 已脱敏服务器API
    full_url[0] = '\0';

    if (strstr(url, "://") != NULL) {
        // Already absolute - just copy and let the variant logic rewrite
        strncpy(full_url, url, max_len - 1);
        full_url[max_len - 1] = '\0';
        return 0;
    }

    if (strncmp(url, "/model/", 7) != 0) {
        snprintf(full_url, max_len, "%s%s", domain, url);
        full_url[max_len - 1] = '\0';
        return 0;
    }

    // url starts with /model/...
    switch (variant) {
        case 0:
            snprintf(full_url, max_len, "%s/api%s", domain, url);
            break;
        case 1:
            // Fallback: /api/p4/model/...
            snprintf(full_url, max_len, "%s/api/p4%s", domain, url);
            break;
        default:
            snprintf(full_url, max_len, "%s%s", domain, url);
            break;
    }
    full_url[max_len - 1] = '\0';
    return 0;
}

static bool download_file_to_sd(const char *url, const char *dest_path)
{
    int retry_limit = 3;
    bool success = false;

    // Make sure parent dir exists
    mkdir(CONFIG_BSP_SD_MOUNT_POINT "/model", 0777);

    for (int attempt = 1; attempt <= retry_limit; attempt++) {
        char full_url[512];
        build_download_url(url, full_url, sizeof(full_url), attempt - 1);

        if (strstr(full_url, "key=") == NULL) {
            if (strstr(full_url, "?") == NULL) {
                if (strlen(full_url) + strlen("?key=bigboss") < sizeof(full_url)) {
                    strcat(full_url, "?key=bigboss");
                }
            } else {
                if (strlen(full_url) + strlen("&key=bigboss") < sizeof(full_url)) {
                    strcat(full_url, "&key=bigboss");
                }
            }
        }
        char buster[32];
        snprintf(buster, sizeof(buster), "&t=%u", (unsigned int)time(NULL));
        if (strlen(full_url) + strlen(buster) < sizeof(full_url)) {
            strcat(full_url, buster);
        }

        ESP_LOGI(MM_TAG, "Downloading file to SD from %s (Attempt %d/%d)...", full_url, attempt, retry_limit);

        esp_http_client_config_t cfg = {};
        cfg.url = full_url;
        cfg.method = HTTP_METHOD_GET;
        cfg.timeout_ms = 30000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(MM_TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(MM_TAG, "HTTP server returned code %d", status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        FILE *f = fopen(dest_path, "wb");
        if (!f) {
            ESP_LOGE(MM_TAG, "Failed to open destination path for write: %s", dest_path);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            break;
        }

        char *chunk_buf = (char *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
        if (!chunk_buf) {
            ESP_LOGE(MM_TAG, "Failed to allocate chunk buffer in PSRAM");
            fclose(f);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            break;
        }

        int read_total = 0;
        bool read_error = false;
        while (1) {
            int read_bytes = esp_http_client_read(client, chunk_buf, 1024);
            if (read_bytes < 0) {
                ESP_LOGE(MM_TAG, "Read error occurred");
                read_error = true;
                break;
            }
            if (read_bytes == 0) {
                break;
            }
            size_t written = fwrite(chunk_buf, 1, read_bytes, f);
            if (written != (size_t)read_bytes) {
                ESP_LOGE(MM_TAG, "Fwrite to SD failed during download");
                read_error = true;
                break;
            }
            read_total += read_bytes;
        }

        heap_caps_free(chunk_buf);
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (read_error || read_total <= 0) {
            unlink(dest_path);
            vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
            continue;
        }

        if (content_len > 0 && read_total < content_len) {
            ESP_LOGE(MM_TAG, "Download short. Read %d, expected %d.", read_total, content_len);
            unlink(dest_path);
            vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
            continue;
        }

        ESP_LOGI(MM_TAG, "File downloaded successfully to SD: %s (%d bytes)", dest_path, read_total);
        success = true;
        break;
    }

    return success;
}

static void format_double_clean(float val, char *buf, size_t buf_sz)
{
    snprintf(buf, buf_sz, "%.2f", val);
    size_t len = strlen(buf);
    while (len > 0 && buf[len - 1] == '0') {
        buf[len - 1] = '\0';
        len--;
    }
    if (len > 0 && buf[len - 1] == '.') {
        buf[len - 1] = '\0';
    }
}

bool model_mgr_load_norm_json(const char *model_name)
{
    // Reset state to fallback default
    for (int i = 0; i < 16; i++) {
        g_model_min[i] = 0.0f;
        g_model_max[i] = 1.0f;
    }
    g_model_num_classes = 0;
    memset(g_model_classes, 0, sizeof(g_model_classes));
    g_model_has_norm = false;

    // Filter model name (strip .tflite if present)
    char base_name[64];
    strncpy(base_name, model_name, sizeof(base_name) - 1);
    base_name[sizeof(base_name) - 1] = '\0';
    char *dot = strstr(base_name, ".tflite");
    if (dot) *dot = '\0';

    char path[256];
    snprintf(path, sizeof(path), CONFIG_BSP_SD_MOUNT_POINT "/model/%s/%s_norm.json", base_name, base_name);

    ESP_LOGI(MM_TAG, "Attempting to load norm parameters from %s", path);

    FILE *f = fopen(path, "r");
    if (!f) {
        // Fallback to legacy flat path
        snprintf(path, sizeof(path), CONFIG_BSP_SD_MOUNT_POINT "/model/%s_norm.json", base_name);
        f = fopen(path, "r");
    }

    if (!f) {
        ESP_LOGW(MM_TAG, "norm.json not found for model %s. Using default MinMaxScaler [0, 1].", base_name);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) {
        fclose(f);
        ESP_LOGE(MM_TAG, "norm.json file is empty");
        return false;
    }

    char *json_buf = (char *)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM);
    if (!json_buf) {
        fclose(f);
        ESP_LOGE(MM_TAG, "Failed to allocate memory in PSRAM for loading norm.json");
        return false;
    }

    size_t read_bytes = fread(json_buf, 1, sz, f);
    fclose(f);
    json_buf[read_bytes] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    heap_caps_free(json_buf);

    if (!root) {
        ESP_LOGE(MM_TAG, "Failed to parse norm.json content");
        return false;
    }

    cJSON *min_arr = cJSON_GetObjectItem(root, "min");
    cJSON *max_arr = cJSON_GetObjectItem(root, "max");
    cJSON *classes_arr = cJSON_GetObjectItem(root, "class_names");

    if (!min_arr || !max_arr || !classes_arr || 
        !cJSON_IsArray(min_arr) || !cJSON_IsArray(max_arr) || !cJSON_IsArray(classes_arr)) {
        ESP_LOGE(MM_TAG, "norm.json is missing required array fields (min/max/class_names)");
        cJSON_Delete(root);
        return false;
    }

    int min_sz = cJSON_GetArraySize(min_arr);
    int max_sz = cJSON_GetArraySize(max_arr);
    if (min_sz != 16 || max_sz != 16) {
        ESP_LOGE(MM_TAG, "Invalid norm.json arrays length (min size=%d, max size=%d, expected 16)", min_sz, max_sz);
        cJSON_Delete(root);
        return false;
    }

    // Extract min & max arrays
    for (int i = 0; i < 16; i++) {
        cJSON *min_val = cJSON_GetArrayItem(min_arr, i);
        cJSON *max_val = cJSON_GetArrayItem(max_arr, i);
        g_model_min[i] = min_val ? (float)min_val->valuedouble : 0.0f;
        g_model_max[i] = max_val ? (float)max_val->valuedouble : 1.0f;
    }

    // Extract class names
    int cls_sz = cJSON_GetArraySize(classes_arr);
    if (cls_sz > 8) cls_sz = 8;
    g_model_num_classes = cls_sz;

    char classes_csv[256] = "";
    for (int i = 0; i < cls_sz; i++) {
        cJSON *cls_val = cJSON_GetArrayItem(classes_arr, i);
        if (cls_val && cls_val->valuestring) {
            strncpy(g_model_classes[i], cls_val->valuestring, sizeof(g_model_classes[i]) - 1);
            g_model_classes[i][sizeof(g_model_classes[i]) - 1] = '\0';
            
            if (i > 0) strcat(classes_csv, ",");
            strcat(classes_csv, cls_val->valuestring);
        }
    }

    // Sync classes back to g_receiver_model_classes for uniform display & reporting
    if (strlen(classes_csv) > 0) {
        strncpy(g_receiver_model_classes, classes_csv, sizeof(g_receiver_model_classes) - 1);
        g_receiver_model_classes[sizeof(g_receiver_model_classes) - 1] = '\0';
        ESP_LOGI(MM_TAG, "Synchronized model classes: %s", g_receiver_model_classes);
    }

    g_model_has_norm = true;
    cJSON_Delete(root);
    ESP_LOGI(MM_TAG, "Successfully loaded norm parameters for model %s", base_name);
    return true;
}

static bool download_model_to_psram(const char *url, int total_size)
{
    int retry_limit = 3;
    bool success = false;

    for (int attempt = 1; attempt <= retry_limit; attempt++) {
        char full_url[512];
        build_download_url(url, full_url, sizeof(full_url), attempt - 1);

        // Auto rewrite backend model.tflite bug
        char *suffix = strstr(full_url, "/model.tflite");
        if (suffix != NULL && strlen(s_model_ctx.model_name) > 0) {
            int base_len = suffix - full_url;
            char name_suffix[128];
            snprintf(name_suffix, sizeof(name_suffix), "/%s.tflite", s_model_ctx.model_name);
            if (base_len + (int)strlen(name_suffix) < (int)sizeof(full_url)) {
                strcpy(suffix, name_suffix);
            }
        }

        if (strstr(full_url, "key=") == NULL) {
            if (strstr(full_url, "?") == NULL) {
                if (strlen(full_url) + strlen("?key=bigboss") < sizeof(full_url)) {
                    strcat(full_url, "?key=bigboss");
                }
            } else {
                if (strlen(full_url) + strlen("&key=bigboss") < sizeof(full_url)) {
                    strcat(full_url, "&key=bigboss");
                }
            }
        }
        char buster[32];
        snprintf(buster, sizeof(buster), "&t=%u", (unsigned int)time(NULL));
        if (strlen(full_url) + strlen(buster) < sizeof(full_url)) {
            strcat(full_url, buster);
        }

        ESP_LOGI(MM_TAG, "Downloading model file from %s (Attempt %d/%d)...", full_url, attempt, retry_limit);

        if (s_model_ctx.file_data) {
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
        }

        int alloc_size = (total_size > 0) ? total_size : DOWNLOAD_GROW_SIZE;
        s_model_ctx.file_data = (uint8_t *)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
        if (!s_model_ctx.file_data) {
            ESP_LOGE(MM_TAG, "Failed to allocate PSRAM (size=%d)", alloc_size);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        esp_http_client_config_t cfg = {};
        cfg.url = full_url;
        cfg.method = HTTP_METHOD_GET;
        cfg.timeout_ms = 30000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(MM_TAG, "Failed to open connection: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
            vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
            continue;
        }

        int content_len = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        if (status_code != 200) {
            ESP_LOGE(MM_TAG, "HTTP download failed with status code %d", status_code);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
            vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
            continue;
        }

        if (content_len > alloc_size) {
            uint8_t *nb = (uint8_t *)heap_caps_realloc(s_model_ctx.file_data, content_len, MALLOC_CAP_SPIRAM);
            if (!nb) {
                ESP_LOGE(MM_TAG, "Realloc failed for size=%d", content_len);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                heap_caps_free(s_model_ctx.file_data);
                s_model_ctx.file_data = NULL;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            s_model_ctx.file_data = nb;
            alloc_size = content_len;
        }

        int read_total = 0;
        bool read_error = false;
        while (1) {
            int remaining = alloc_size - read_total;
            if (remaining <= 0) {
                alloc_size += DOWNLOAD_GROW_SIZE;
                uint8_t *nb = (uint8_t *)heap_caps_realloc(s_model_ctx.file_data, alloc_size, MALLOC_CAP_SPIRAM);
                if (!nb) {
                    ESP_LOGE(MM_TAG, "Grow realloc failed at size=%d", read_total);
                    read_error = true;
                    break;
                }
                s_model_ctx.file_data = nb;
                remaining = DOWNLOAD_GROW_SIZE;
            }

            int r = esp_http_client_read(client, (char *)s_model_ctx.file_data + read_total, remaining);
            if (r <= 0) {
                if (r == -ESP_ERR_HTTP_EAGAIN) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
                if (r < 0) {
                    ESP_LOGE(MM_TAG, "Read error: %d", r);
                    read_error = true;
                }
                break;
            }
            read_total += r;
            if (content_len > 0) {
                s_model_ctx.current_chunk = read_total;
                s_model_ctx.total_chunks = content_len;
            }
            post_progress(s_model_ctx.model_name, content_len, read_total);
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (read_error || read_total <= 0) {
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
            vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
            continue;
        }

        if (content_len > 0 && read_total < content_len) {
            ESP_LOGE(MM_TAG, "Download short. Read %d, expected %d.", read_total, content_len);
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
            vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
            continue;
        }

        s_model_ctx.total_size = read_total;
        s_model_ctx.total_chunks = (read_total + MODEL_CHUNK_SIZE - 1) / MODEL_CHUNK_SIZE;
        ESP_LOGI(MM_TAG, "Model download completed: %d bytes, %d chunks", read_total, s_model_ctx.total_chunks);

        if (!is_valid_tflite(s_model_ctx.file_data, read_total)) {
            ESP_LOGE(MM_TAG, "Downloaded file is not a valid TFLite model! (Missing 'TFL3' magic bytes)");
            log_invalid_model_header(s_model_ctx.file_data, read_total);
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
            vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
            continue;
        }

        success = true;
        break;
    }

    return success;
}

static bool save_model_to_sd(void)
{
    if (!s_model_ctx.file_data || s_model_ctx.total_size <= 0) {
        ESP_LOGE(MM_TAG, "No model data to save to SD card");
        return false;
    }

    // 确保 SD 卡已挂载
    if (!sd_card_bg_is_ready()) {
        esp_err_t err = sd_card_bg_init();
        if (err != ESP_OK) {
            ESP_LOGE(MM_TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
            return false;
        }
    }

    // 防御性创建目录，防止文件夹不存在时写入失败
    mkdir(CONFIG_BSP_SD_MOUNT_POINT "/model", 0777);

    // Create subfolder for this model
    char base_name[64];
    strncpy(base_name, s_model_ctx.model_name, sizeof(base_name) - 1);
    base_name[sizeof(base_name) - 1] = '\0';
    char *dot = strstr(base_name, ".tflite");
    if (dot) *dot = '\0';

    char model_dir[256];
    snprintf(model_dir, sizeof(model_dir), CONFIG_BSP_SD_MOUNT_POINT "/model/%s", base_name);
    mkdir(model_dir, 0777);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.tflite", model_dir, base_name);

    ESP_LOGI(MM_TAG, "Saving model to SD card at %s (size=%d)...", path, s_model_ctx.total_size);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(MM_TAG, "Failed to open file on SD card: %s", path);
        return false;
    }

    size_t written = fwrite(s_model_ctx.file_data, 1, s_model_ctx.total_size, f);
    fclose(f);

    if (written != (size_t)s_model_ctx.total_size) {
        ESP_LOGE(MM_TAG, "Fwrite short. Written %u, expected %d", (unsigned int)written, s_model_ctx.total_size);
        unlink(path); // 写入破损，删除不完整文件
        return false;
    }

    ESP_LOGI(MM_TAG, "Model successfully saved to SD card: %s", path);
    return true;
}

static void model_mgr_send_metadata(void)
{
    char device_name[32] = "";
    if (!uart_receiver_get_first_paired_name(device_name, sizeof(device_name)) || strlen(device_name) == 0) {
        strncpy(device_name, "S3_01", sizeof(device_name) - 1);
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "model update %s %d %d %s",
             s_model_ctx.model_name, s_model_ctx.total_chunks, s_model_ctx.total_size, s_model_ctx.model_version);

    ESP_LOGI(MM_TAG, "Sending metadata to C6 for %s: %s", device_name, buf);
    uart_receiver_send_cmd(device_name, buf);

    s_model_ctx.state = MCS_SEND_META;
    s_model_ctx.last_action_time = xTaskGetTickCount();
    s_model_ctx.retry_count = 0;
}

static void model_mgr_send_chunk(int chunk_id)
{
    if (!s_model_ctx.file_data || chunk_id >= s_model_ctx.total_chunks) return;

    int offset = chunk_id * MODEL_CHUNK_SIZE;
    int chunk_len = s_model_ctx.total_size - offset;
    if (chunk_len > MODEL_CHUNK_SIZE) chunk_len = MODEL_CHUNK_SIZE;

    char *hex_buf = (char *)heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    if (!hex_buf) {
        ESP_LOGE(MM_TAG, "Failed to allocate hex buffer");
        return;
    }

    bytes_to_hex(s_model_ctx.file_data + offset, chunk_len, hex_buf);

    char device_name[32] = "";
    if (!uart_receiver_get_first_paired_name(device_name, sizeof(device_name)) || strlen(device_name) == 0) {
        strncpy(device_name, "S3_01", sizeof(device_name) - 1);
    }

    char *cmd_buf = (char *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (cmd_buf) {
        snprintf(cmd_buf, 1024, "model chunk %d %s", chunk_id, hex_buf);
        ESP_LOGI(MM_TAG, "Sending chunk %d/%d (len=%d) via C6 to %s", chunk_id + 1, s_model_ctx.total_chunks, chunk_len, device_name);
        uart_receiver_send_cmd(device_name, cmd_buf);
        heap_caps_free(cmd_buf);
    }

    heap_caps_free(hex_buf);

    s_model_ctx.current_chunk = chunk_id;
    s_model_ctx.last_action_time = xTaskGetTickCount();
}

void model_mgr_handle_s3_msg(const char *mt_str, int mid)
{
    if (s_model_ctx.state == MCS_IDLE) return;

    if (strcmp(mt_str, "chunk_ok") == 0 || strcmp(mt_str, "ack") == 0 || strcmp(mt_str, "ready") == 0) {
        if (s_model_ctx.state == MCS_SEND_META && (mid == 0 || strcmp(mt_str, "ready") == 0)) {
            ESP_LOGI(MM_TAG, "S3 received metadata (confirmed by %s). Starting chunk transmission...", mt_str);
            s_model_ctx.state = MCS_SENDING_CHUNKS;
            s_model_ctx.retry_count = 0;
            model_mgr_send_chunk(0);
        } else if (s_model_ctx.state == MCS_SENDING_CHUNKS && mid == s_model_ctx.current_chunk) {
            ESP_LOGI(MM_TAG, "S3 confirmed chunk %d (by %s). Sending next...", mid, mt_str);
            s_model_ctx.retry_count = 0;

            // Report flash OTA progress to server - POST to /p4/submit type=model_download_progress
            int done_bytes = (mid + 1) * MODEL_CHUNK_SIZE;
            if (done_bytes > s_model_ctx.total_size) done_bytes = s_model_ctx.total_size;
            post_progress(s_model_ctx.model_name, s_model_ctx.total_size, done_bytes);

            int next_chunk = mid + 1;
            if (next_chunk < s_model_ctx.total_chunks) {
                vTaskDelay(pdMS_TO_TICKS(150));
                model_mgr_send_chunk(next_chunk);
            } else {
                ESP_LOGI(MM_TAG, "All chunks sent. Waiting for reload confirmation...");
                s_model_ctx.state = MCS_WAIT_DONE;
                s_model_ctx.last_action_time = xTaskGetTickCount();
            }
        } else if (s_model_ctx.state == MCS_SENDING_NORM) {
            ESP_LOGI(MM_TAG, "S3 confirmed norm metadata (by %s). Sending class names...", mt_str);
            char device_name[32] = "";
            if (!uart_receiver_get_first_paired_name(device_name, sizeof(device_name)) || strlen(device_name) == 0) {
                strncpy(device_name, "S3_01", sizeof(device_name) - 1);
            }
            char classes_csv[192] = "";
            for (int i = 0; i < g_model_num_classes; i++) {
                strcat(classes_csv, g_model_classes[i]);
                if (i < g_model_num_classes - 1) strcat(classes_csv, ",");
            }
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "model classes %s %s", s_model_ctx.model_name, classes_csv);
            ESP_LOGI(MM_TAG, "Sending class names to %s: %s", device_name, cmd);
            uart_receiver_send_cmd(device_name, cmd);

            s_model_ctx.state = MCS_SENDING_CLASSES;
            s_model_ctx.last_action_time = xTaskGetTickCount();
            s_model_ctx.retry_count = 0;
        }
    } else if ((strcmp(mt_str, "model_done") == 0 || strcmp(mt_str, "done") == 0) && 
               (s_model_ctx.state == MCS_WAIT_DONE || s_model_ctx.state == MCS_SENDING_CLASSES)) {
        
        if (s_model_ctx.state == MCS_WAIT_DONE) {
            ESP_LOGI(MM_TAG, "Receiver model binary update completed! Confirmed by %s. Checking norm parameter status...", mt_str);
            
            // Try loading normalization parameters locally first
            model_mgr_load_norm_json(s_model_ctx.model_name);

            if (g_model_has_norm) {
                char device_name[32] = "";
                if (!uart_receiver_get_first_paired_name(device_name, sizeof(device_name)) || strlen(device_name) == 0) {
                    strncpy(device_name, "S3_01", sizeof(device_name) - 1);
                }
                char min_csv[128] = "";
                char max_csv[128] = "";
                for (int i = 0; i < 16; i++) {
                    char fbuf[16];
                    format_double_clean(g_model_min[i], fbuf, sizeof(fbuf));
                    strcat(min_csv, fbuf);
                    if (i < 15) strcat(min_csv, ",");

                    format_double_clean(g_model_max[i], fbuf, sizeof(fbuf));
                    strcat(max_csv, fbuf);
                    if (i < 15) strcat(max_csv, ",");
                }
                char cmd[384];
                snprintf(cmd, sizeof(cmd), "model norm %s %s %s", s_model_ctx.model_name, min_csv, max_csv);
                ESP_LOGI(MM_TAG, "Norm files found. Commencing norm parameters transmission to %s: %s", device_name, cmd);
                uart_receiver_send_cmd(device_name, cmd);

                s_model_ctx.state = MCS_SENDING_NORM;
                s_model_ctx.last_action_time = xTaskGetTickCount();
                s_model_ctx.retry_count = 0;
                return; // Wait for norm ACK
            } else {
                ESP_LOGW(MM_TAG, "No norm parameters found locally for %s. Skipping S3 config transfer.", s_model_ctx.model_name);
            }
        }

        ESP_LOGI(MM_TAG, "Receiver model update fully completed (state=%d)! Confirmed by %s", s_model_ctx.state, mt_str);
        
        // 网关本地同步更新当前激活模型的名称、版本与就绪状态，使屏幕即时呈现
        strncpy(g_receiver_model_name, s_model_ctx.model_name, sizeof(g_receiver_model_name) - 1);
        g_receiver_model_name[sizeof(g_receiver_model_name) - 1] = '\0';
        strncpy(g_receiver_model_version, s_model_ctx.model_version, sizeof(g_receiver_model_version) - 1);
        g_receiver_model_version[sizeof(g_receiver_model_version) - 1] = '\0';
        g_receiver_model_ready = true;

        // Ensure norm parameters are loaded locally (in case we skipped S3 config transfer)
        model_mgr_load_norm_json(s_model_ctx.model_name);
 
        // 触发一次云端最新模型上报
        cloud_sync_trigger_model_info_upload();
 
        submit_update_result(s_model_ctx.model_name, "success", "");
        if (s_model_ctx.file_data) {
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
        }
        s_model_ctx.state = MCS_IDLE;
    } else if (strcmp(mt_str, "model_fail") == 0 || strcmp(mt_str, "error") == 0) {
        ESP_LOGE(MM_TAG, "Receiver reported model failure! Confirmed by %s", mt_str);
        submit_update_result(s_model_ctx.model_name, "fail", strcmp(mt_str, "error") == 0 ? "Receiver reported error" : "Receiver reported model_fail");
        if (s_model_ctx.file_data) {
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
        }
        s_model_ctx.state = MCS_IDLE;
    }
}

void model_mgr_check_timeout(void)
{
    if (s_model_ctx.state == MCS_IDLE || s_model_ctx.state == MCS_DOWNLOADING) return;

    uint32_t now = xTaskGetTickCount();
    if (now - s_model_ctx.last_action_time < pdMS_TO_TICKS(MODEL_CMD_TIMEOUT)) return;

    ESP_LOGW(MM_TAG, "Timeout. Retry count: %d", s_model_ctx.retry_count);

    if (s_model_ctx.retry_count >= 3) {
        ESP_LOGE(MM_TAG, "Max retries exceeded. Aborting.");
        submit_update_result(s_model_ctx.model_name, "timeout", "Max retry exceeded");
        if (s_model_ctx.file_data) {
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
        }
        s_model_ctx.state = MCS_IDLE;
        return;
    }

    s_model_ctx.retry_count++;
    s_model_ctx.last_action_time = now;

    if (s_model_ctx.state == MCS_SEND_META) {
        char device_name[32] = "";
        if (!uart_receiver_get_first_paired_name(device_name, sizeof(device_name)) || strlen(device_name) == 0) {
            strncpy(device_name, "S3_01", sizeof(device_name) - 1);
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "model update %s %d %d %s",
                 s_model_ctx.model_name, s_model_ctx.total_chunks, s_model_ctx.total_size, s_model_ctx.model_version);
        ESP_LOGI(MM_TAG, "Retrying metadata to C6 for %s: %s", device_name, buf);
        uart_receiver_send_cmd(device_name, buf);
    } else if (s_model_ctx.state == MCS_SENDING_CHUNKS) {
        ESP_LOGI(MM_TAG, "Retrying chunk: %d", s_model_ctx.current_chunk);
        model_mgr_send_chunk(s_model_ctx.current_chunk);
    } else if (s_model_ctx.state == MCS_WAIT_DONE) {
        ESP_LOGE(MM_TAG, "Timeout waiting for reload done!");
        submit_update_result(s_model_ctx.model_name, "timeout", "Timeout waiting for reload");
        if (s_model_ctx.file_data) {
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
        }
        s_model_ctx.state = MCS_IDLE;
    } else if (s_model_ctx.state == MCS_SENDING_NORM) {
        char device_name[32] = "";
        if (!uart_receiver_get_first_paired_name(device_name, sizeof(device_name)) || strlen(device_name) == 0) {
            strncpy(device_name, "S3_01", sizeof(device_name) - 1);
        }
        char min_csv[128] = "";
        char max_csv[128] = "";
        for (int i = 0; i < 16; i++) {
            char fbuf[16];
            format_double_clean(g_model_min[i], fbuf, sizeof(fbuf));
            strcat(min_csv, fbuf);
            if (i < 15) strcat(min_csv, ",");

            format_double_clean(g_model_max[i], fbuf, sizeof(fbuf));
            strcat(max_csv, fbuf);
            if (i < 15) strcat(max_csv, ",");
        }
        char cmd[384];
        snprintf(cmd, sizeof(cmd), "model norm %s %s %s", s_model_ctx.model_name, min_csv, max_csv);
        ESP_LOGI(MM_TAG, "Retrying norm command to %s: %s", device_name, cmd);
        uart_receiver_send_cmd(device_name, cmd);
    } else if (s_model_ctx.state == MCS_SENDING_CLASSES) {
        char device_name[32] = "";
        if (!uart_receiver_get_first_paired_name(device_name, sizeof(device_name)) || strlen(device_name) == 0) {
            strncpy(device_name, "S3_01", sizeof(device_name) - 1);
        }
        char classes_csv[192] = "";
        for (int i = 0; i < g_model_num_classes; i++) {
            strcat(classes_csv, g_model_classes[i]);
            if (i < g_model_num_classes - 1) strcat(classes_csv, ",");
        }
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "model classes %s %s", s_model_ctx.model_name, classes_csv);
        ESP_LOGI(MM_TAG, "Retrying classes command to %s: %s", device_name, cmd);
        uart_receiver_send_cmd(device_name, cmd);
    }
}

void model_mgr_trigger_update(const char *url, const char *norm_url, const char *name, const char *version, const char *cmd_id)
{
    if (s_model_ctx.state != MCS_IDLE) {
        ESP_LOGW(MM_TAG, "Model updater busy (state=%d)", s_model_ctx.state);
        return;
    }

    strncpy(s_model_ctx.model_name, name, sizeof(s_model_ctx.model_name) - 1);
    s_model_ctx.model_name[sizeof(s_model_ctx.model_name) - 1] = '\0';

    strncpy(s_model_ctx.model_version, version, sizeof(s_model_ctx.model_version) - 1);
    s_model_ctx.model_version[sizeof(s_model_ctx.model_version) - 1] = '\0';

    strncpy(s_model_ctx.model_url, url, sizeof(s_model_ctx.model_url) - 1);
    s_model_ctx.model_url[sizeof(s_model_ctx.model_url) - 1] = '\0';

    if (norm_url) {
        strncpy(s_model_ctx.norm_url, norm_url, sizeof(s_model_ctx.norm_url) - 1);
        s_model_ctx.norm_url[sizeof(s_model_ctx.norm_url) - 1] = '\0';
    } else {
        s_model_ctx.norm_url[0] = '\0';
    }

    strncpy(s_model_ctx.cmd_id, cmd_id ? cmd_id : "", sizeof(s_model_ctx.cmd_id) - 1);
    s_model_ctx.cmd_id[sizeof(s_model_ctx.cmd_id) - 1] = '\0';

    s_model_ctx.state = MCS_DOWNLOADING;
    ESP_LOGI(MM_TAG, "Model update triggered. Name: %s, Ver: %s, URL: %s, Norm URL: %s", name, version, url, s_model_ctx.norm_url);

    s_model_ctx.total_size = 0;
    s_model_ctx.total_chunks = 0;
    s_model_ctx.current_chunk = 0;

    if (download_model_to_psram(url, 0)) {
        ESP_LOGI(MM_TAG, "Model download completed. Saving to local SD card...");
        save_model_to_sd(); // 自动持久化保存到本地 SD 卡 model/ 目录下

        // Download norm.json if provided
        if (strlen(s_model_ctx.norm_url) > 0) {
            char norm_path[256];
            char base_name[64];
            strncpy(base_name, s_model_ctx.model_name, sizeof(base_name) - 1);
            base_name[sizeof(base_name) - 1] = '\0';
            char *dot = strstr(base_name, ".tflite");
            if (dot) *dot = '\0';

            char model_dir[256];
            snprintf(model_dir, sizeof(model_dir), CONFIG_BSP_SD_MOUNT_POINT "/model/%s", base_name);
            mkdir(model_dir, 0777);

            snprintf(norm_path, sizeof(norm_path), "%s/%s_norm.json", model_dir, base_name);

            ESP_LOGI(MM_TAG, "Downloading norm.json from %s to %s...", s_model_ctx.norm_url, norm_path);
            if (!download_file_to_sd(s_model_ctx.norm_url, norm_path)) {
                ESP_LOGW(MM_TAG, "Failed to download norm.json! Proceeding with model only.");
            }
        }
        
        // 释放 PSRAM 临时缓冲区内存，并结束升级流程，不自动进行握手和发送
        if (s_model_ctx.file_data) {
            heap_caps_free(s_model_ctx.file_data);
            s_model_ctx.file_data = NULL;
        }
        submit_update_result(s_model_ctx.model_name, "success", "Model and norm downloaded and saved to local SD");
        s_model_ctx.state = MCS_IDLE;
    } else {
        ESP_LOGE(MM_TAG, "Failed to download model file from cloud!");
        submit_update_result(s_model_ctx.model_name, "fail", "Failed to download model file");
        s_model_ctx.state = MCS_IDLE;
    }
}

void model_mgr_get_state(struct ModelMgrStateInfo *info)
{
    if (!info) return;
    strncpy(info->name, s_model_ctx.model_name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    strncpy(info->version, s_model_ctx.model_version, sizeof(info->version) - 1);
    info->version[sizeof(info->version) - 1] = '\0';
    info->state = s_model_ctx.state;
    info->total_size = s_model_ctx.total_size;
    if (s_model_ctx.total_chunks > 0) {
        info->progress_percent = (s_model_ctx.current_chunk * 100) / s_model_ctx.total_chunks;
    } else {
        info->progress_percent = 0;
    }
}

static bool model_mgr_load_and_send_sd_model_depth(const char *model_name, int depth);

bool model_mgr_load_and_send_sd_model(const char *model_name)
{
    return model_mgr_load_and_send_sd_model_depth(model_name, 0);
}

bool model_mgr_delete_local_model(const char *model_name)
{
    if (!model_name || strlen(model_name) == 0) return false;

    char sub_tflite[256];
    char sub_norm[256];
    char sub_dir[256];
    snprintf(sub_tflite, sizeof(sub_tflite), CONFIG_BSP_SD_MOUNT_POINT "/model/%s/%s.tflite", model_name, model_name);
    snprintf(sub_norm, sizeof(sub_norm), CONFIG_BSP_SD_MOUNT_POINT "/model/%s/%s_norm.json", model_name, model_name);
    snprintf(sub_dir, sizeof(sub_dir), CONFIG_BSP_SD_MOUNT_POINT "/model/%s", model_name);

    bool deleted = false;

    if (unlink(sub_tflite) == 0) {
        ESP_LOGI(MM_TAG, "Deleted subdirectory model file: %s", sub_tflite);
        deleted = true;
    }
    if (unlink(sub_norm) == 0) {
        ESP_LOGI(MM_TAG, "Deleted subdirectory norm file: %s", sub_norm);
        deleted = true;
    }
    if (rmdir(sub_dir) == 0) {
        ESP_LOGI(MM_TAG, "Removed subdirectory: %s", sub_dir);
        deleted = true;
    }

    char flat_tflite[256];
    char flat_norm[256];
    snprintf(flat_tflite, sizeof(flat_tflite), CONFIG_BSP_SD_MOUNT_POINT "/model/%s.tflite", model_name);
    snprintf(flat_norm, sizeof(flat_norm), CONFIG_BSP_SD_MOUNT_POINT "/model/%s_norm.json", model_name);

    if (unlink(flat_tflite) == 0) {
        ESP_LOGI(MM_TAG, "Deleted flat model file: %s", flat_tflite);
        deleted = true;
    }
    if (unlink(flat_norm) == 0) {
        ESP_LOGI(MM_TAG, "Deleted flat norm file: %s", flat_norm);
        deleted = true;
    }

    return deleted;
}
 
static bool model_mgr_load_and_send_sd_model_depth(const char *model_name, int depth)
{
    if (depth > 1) {
        ESP_LOGE(MM_TAG, "Too many fallback attempts for model '%s'", model_name);
        return false;
    }
    if (s_model_ctx.state != MCS_IDLE) {
        ESP_LOGW(MM_TAG, "Model manager busy, cannot upload from SD (state=%d)", s_model_ctx.state);
        return false;
    }

    char base[64];
    strncpy(base, model_name, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    char *dot = strstr(base, ".tflite");
    if (dot) *dot = '\0';

    char path[256];
    snprintf(path, sizeof(path), CONFIG_BSP_SD_MOUNT_POINT "/model/%s/%s.tflite", base, base);

    FILE *f = fopen(path, "rb");
    if (!f) {
        // Fallback to legacy flat path
        if (strstr(model_name, ".tflite") != NULL) {
            snprintf(path, sizeof(path), CONFIG_BSP_SD_MOUNT_POINT "/model/%s", model_name);
        } else {
            snprintf(path, sizeof(path), CONFIG_BSP_SD_MOUNT_POINT "/model/%s.tflite", model_name);
        }
        f = fopen(path, "rb");
    }

    if (!f) {
        ESP_LOGW(MM_TAG, "SD model not found locally: %s. Attempting cloud download...", path);
        char dl_url[256];
        char norm_url[256];
        snprintf(dl_url, sizeof(dl_url), "/model/download/%s/%s.tflite", base, base);
        snprintf(norm_url, sizeof(norm_url), "/model/download/%s/%s_norm.json", base, base);
        model_mgr_trigger_update(dl_url, norm_url, base, "1.0.0", "");
        return model_mgr_load_and_send_sd_model_depth(model_name, depth + 1);
    }

    // 获取文件大小
    fseek(f, 0, SEEK_END);
    int total_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (total_size <= 0) {
        ESP_LOGE(MM_TAG, "Invalid SD model file size (%d): %s", total_size, path);
        fclose(f);
        return false;
    }

    // 分配 PSRAM 缓存
    if (s_model_ctx.file_data) {
        heap_caps_free(s_model_ctx.file_data);
        s_model_ctx.file_data = NULL;
    }
    s_model_ctx.file_data = (uint8_t *)heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM);
    if (!s_model_ctx.file_data) {
        ESP_LOGE(MM_TAG, "Failed to allocate PSRAM for local SD model (size=%d)", total_size);
        fclose(f);
        return false;
    }

    size_t read_bytes = fread(s_model_ctx.file_data, 1, total_size, f);
    fclose(f);

    if (read_bytes != (size_t)total_size) {
        ESP_LOGE(MM_TAG, "Read short from SD model: read %d, expected %d", (int)read_bytes, total_size);
        heap_caps_free(s_model_ctx.file_data);
        s_model_ctx.file_data = NULL;
        return false;
    }

    if (!is_valid_tflite(s_model_ctx.file_data, total_size)) {
        ESP_LOGE(MM_TAG, "Loaded SD model '%s' is not a valid TFLite model! (Missing 'TFL3' magic bytes)", model_name);
        log_invalid_model_header(s_model_ctx.file_data, total_size);
        heap_caps_free(s_model_ctx.file_data);
        s_model_ctx.file_data = NULL;

        // SD 文件无效，删除后尝试从云端下载
        ESP_LOGW(MM_TAG, "Deleting invalid SD model and falling back to cloud download...");
        unlink(path);

        char dl_url[256];
        char norm_url[256];
        char base[64];
        strncpy(base, model_name, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        char *dot = strstr(base, ".tflite");
        if (dot) *dot = '\0';
        snprintf(dl_url, sizeof(dl_url), "/model/download/%s/%s.tflite", base, base);
        snprintf(norm_url, sizeof(norm_url), "/model/download/%s/%s_norm.json", base, base);

        // Delegate to download + save flow
        model_mgr_trigger_update(dl_url, norm_url, base, "1.0.0", "");

        // Retry loading and sending after download completes
        return model_mgr_load_and_send_sd_model_depth(model_name, depth + 1);
    }

    // 过滤掉文件名里的 .tflite 后缀
    char base_name[64];
    strncpy(base_name, model_name, sizeof(base_name) - 1);
    base_name[sizeof(base_name) - 1] = '\0';
    char *dot_ext = strstr(base_name, ".tflite");
    if (dot_ext) {
        *dot_ext = '\0';
    }

    strncpy(s_model_ctx.model_name, base_name, sizeof(s_model_ctx.model_name) - 1);
    s_model_ctx.model_name[sizeof(s_model_ctx.model_name) - 1] = '\0';
    strncpy(s_model_ctx.model_version, "1.0.0", sizeof(s_model_ctx.model_version) - 1); // 默认版本
    s_model_ctx.model_url[0] = '\0'; // 本地加载，无 URL
    s_model_ctx.cmd_id[0] = '\0';

    s_model_ctx.total_size = total_size;
    s_model_ctx.total_chunks = (total_size + MODEL_CHUNK_SIZE - 1) / MODEL_CHUNK_SIZE;
    s_model_ctx.current_chunk = 0;
    s_model_ctx.retry_count = 0;

    ESP_LOGI(MM_TAG, "Loaded local SD model '%s' (%d bytes) into PSRAM. Commencing S3 metadata handshake...",
             s_model_ctx.model_name, total_size);

    model_mgr_load_norm_json(s_model_ctx.model_name);
    model_mgr_send_metadata();
    return true;
}
