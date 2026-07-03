/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <time.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "esp_lv_adapter.h"
#include "lvgl_adapter_init.h"
#include "esp_ldo_regulator.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "app_video.h"
#include "app_system.h"
#include "ui.h"
#include "web_dashboard.h"
#include "uart_receiver.h"
#include "cloud_sync.h"
#include "xiaozhi_ai_service.h"
#include "bme680.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "csv_buffer.h"
#include "model_mgr.h"

static const char *TAG = "E_NOSE_MAIN";

// Audio test - disabled for production
#define AUDIO_TEST_ENABLED 0

extern "C" void ui_update_xiaozhi_text(const char* text);

// XiaoZhi AI state callback
static void xiaozhi_state_callback(xiaozhi_state_t state, const char* message)
{
    ESP_LOGI(TAG, "XiaoZhi State: %d - %s", state, message ? message : "");
}

// XiaoZhi AI text callback
static void xiaozhi_text_callback(const char* text)
{
    ESP_LOGI(TAG, "Jully Text: %s", text ? text : "");
    ui_update_xiaozhi_text(text);

    if (text) {
        // 1. 转化为小写，以便支持不区分大小写的文件和指令匹配
        char *lower_text = strdup(text);
        if (lower_text) {
            for (int i = 0; lower_text[i]; i++) {
                if (lower_text[i] >= 'A' && lower_text[i] <= 'Z') {
                    lower_text[i] = lower_text[i] - 'A' + 'a';
                }
            }

            // 2. 识别各个外设的关键词
            bool has_uv = strstr(lower_text, "uv") != NULL || strstr(text, "紫外") != NULL || strstr(text, "杀菌") != NULL || strstr(text, "消杀") != NULL;
            bool has_fog = strstr(lower_text, "fog") != NULL || strstr(text, "加湿") != NULL || strstr(text, "雾化") != NULL;
            bool has_fan = strstr(lower_text, "fan") != NULL || strstr(text, "风扇") != NULL || strstr(text, "排风") != NULL || strstr(text, "排气") != NULL || strstr(text, "通风") != NULL;

            // 3. 处理紫外消杀灯 (UV) 意图
            if (has_uv) {
                bool trigger_on = false;
                bool trigger_off = false;
                if (strstr(lower_text, "开") || strstr(lower_text, "启") || strstr(lower_text, "动") || strstr(lower_text, "亮")) {
                    trigger_on = true;
                }
                if (strstr(lower_text, "关") || strstr(lower_text, "闭") || strstr(lower_text, "断") || strstr(lower_text, "停") || strstr(lower_text, "灭")) {
                    trigger_off = true;
                    trigger_on = false; // 关闭优先
                }

                if (trigger_on) {
                    ESP_LOGI(TAG, "Jully Action Interceptor: Triggering UV ON...");
                    char dev[32] = "";
                    uart_receiver_get_first_paired_name(dev, sizeof(dev));
                    if (dev[0]) uart_receiver_send_cmd(dev, "uv_on");
                } else if (trigger_off) {
                    ESP_LOGI(TAG, "Jully Action Interceptor: Triggering UV OFF...");
                    char dev[32] = "";
                    uart_receiver_get_first_paired_name(dev, sizeof(dev));
                    if (dev[0]) uart_receiver_send_cmd(dev, "uv_off");
                }
            }

            // 4. 处理雾化加湿器 (Fogger) 意图
            if (has_fog) {
                bool trigger_on = false;
                bool trigger_off = false;
                if (strstr(lower_text, "开") || strstr(lower_text, "启") || strstr(lower_text, "动") || strstr(lower_text, "亮")) {
                    trigger_on = true;
                }
                if (strstr(lower_text, "关") || strstr(lower_text, "闭") || strstr(lower_text, "断") || strstr(lower_text, "停") || strstr(lower_text, "灭")) {
                    trigger_off = true;
                    trigger_on = false; // 关闭优先
                }

                if (trigger_on) {
                    ESP_LOGI(TAG, "Jully Action Interceptor: Triggering Fogger ON...");
                    char dev[32] = "";
                    uart_receiver_get_first_paired_name(dev, sizeof(dev));
                    if (dev[0]) uart_receiver_send_cmd(dev, "fog_on");
                } else if (trigger_off) {
                    ESP_LOGI(TAG, "Jully Action Interceptor: Triggering Fogger OFF...");
                    char dev[32] = "";
                    uart_receiver_get_first_paired_name(dev, sizeof(dev));
                    if (dev[0]) uart_receiver_send_cmd(dev, "fog_off");
                }
            }

            // 5. 处理风扇 (Fan) 意图
            if (has_fan) {
                bool trigger_on = false;
                bool trigger_off = false;
                if (strstr(lower_text, "开") || strstr(lower_text, "启") || strstr(lower_text, "动") || strstr(lower_text, "亮") || strstr(lower_text, "吹")) {
                    trigger_on = true;
                }
                if (strstr(lower_text, "关") || strstr(lower_text, "闭") || strstr(lower_text, "断") || strstr(lower_text, "停") || strstr(lower_text, "灭")) {
                    trigger_off = true;
                    trigger_on = false; // 关闭优先
                }

                if (trigger_on) {
                    ESP_LOGI(TAG, "Jully Action Interceptor: Triggering Fan ON...");
                    char dev[32] = "";
                    uart_receiver_get_first_paired_name(dev, sizeof(dev));
                    if (dev[0]) uart_receiver_send_cmd(dev, "fan_on");
                } else if (trigger_off) {
                    ESP_LOGI(TAG, "Jully Action Interceptor: Triggering Fan OFF...");
                    char dev[32] = "";
                    uart_receiver_get_first_paired_name(dev, sizeof(dev));
                    if (dev[0]) uart_receiver_send_cmd(dev, "fan_off");
                }
            }

            free(lower_text);
        }
    }
}

// Volcano AI RTC Service
#define VOLCANO_RTC_ENABLED 0
#if VOLCANO_RTC_ENABLED
#include "volcano_rtc_service.h"

static void volcano_rtc_state_callback(volcano_rtc_state_t state, const char* message)
{
    ESP_LOGI(TAG, "Volcano RTC State: %d - %s", state, message ? message : "");
}
#endif



#include "cJSON.h"
#include "sd_card_bg.h"

static const char *AI_HISTORY_FILE = CONFIG_BSP_SD_MOUNT_POINT "/ai_history.log";

static bool ensure_sd_mounted(void)
{
    if (sd_card_bg_is_ready()) return true;
    esp_err_t err = sd_card_bg_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed via sd_card_bg: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void save_ai_result_to_sd(const char *result)
{
    if (!result) return;
    if (!ensure_sd_mounted()) return;

    FILE *f = fopen(AI_HISTORY_FILE, "a");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s for append", AI_HISTORY_FILE);
        return;
    }

    cJSON *entry = cJSON_CreateObject();
    if (!entry) {
        fclose(f);
        return;
    }
    cJSON_AddNumberToObject(entry, "ts", (double)time(NULL));
    cJSON_AddStringToObject(entry, "result", result);

    char *line = cJSON_PrintUnformatted(entry);
    if (line) {
        fprintf(f, "%s\n", line);
        free(line);
    }
    cJSON_Delete(entry);
    fclose(f);

    ESP_LOGI(TAG, "Saved AI analysis to %s", AI_HISTORY_FILE);
}

extern "C" const char* ui_get_ai_history_json(void)
{
    ensure_sd_mounted();

    FILE *f = fopen(AI_HISTORY_FILE, "r");
    if (!f) return strdup("[]");

    char *fbuf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!fbuf) {
        fclose(f);
        return strdup("[]");
    }

    cJSON *arr = cJSON_CreateArray();
    while (fgets(fbuf, 4096, f)) {
        size_t len = strlen(fbuf);
        if (len > 0 && fbuf[len - 1] == '\n') fbuf[len - 1] = '\0';
        if (fbuf[0] == '\0') continue;
        cJSON *entry = cJSON_Parse(fbuf);
        if (entry) {
            cJSON_AddItemToArray(arr, entry);
        }
    }
    fclose(f);
    free(fbuf);

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json ? json : strdup("[]");
}

// Electronic Nose Gateway callbacks and self-test logic removed permanently

#if AUDIO_TEST_ENABLED
#include <math.h>

static void audio_mic_test_task(void *pvParameters)
{
    ESP_LOGI("AUDIO_TEST", "=== MIC Test Started ===");
    
    // Read audio data from microphone
    int16_t audio_buffer[960]; // 60ms @ 16kHz, 16bit, mono
    int test_count = 0;
    int max_tests = 20;
    
    while (test_count < max_tests) {
        size_t bytes_read = 0;
        esp_err_t ret = bsp_extra_i2s_read(audio_buffer, sizeof(audio_buffer), &bytes_read, 1000);
        
        if (ret == ESP_OK && bytes_read > 0) {
            // Calculate RMS of first few samples
            int32_t sum = 0;
            int sample_count = bytes_read / 2;
            if (sample_count > 20) sample_count = 20;
            
            for (int i = 0; i < sample_count; i++) {
                sum += audio_buffer[i] * audio_buffer[i];
            }
            int32_t rms = sqrt(sum / sample_count);
            
            ESP_LOGI("AUDIO_TEST", "MIC read %d bytes, RMS: %ld", bytes_read, rms);
        } else {
            ESP_LOGE("AUDIO_TEST", "MIC read failed: %s", esp_err_to_name(ret));
        }
        
        test_count++;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    ESP_LOGI("AUDIO_TEST", "=== MIC Test Completed ===");
    vTaskDelete(NULL);
}

static void audio_speaker_test_task(void *pvParameters)
{
    ESP_LOGI("AUDIO_TEST", "=== Speaker Test Started ===");
    
    // Enable PA (Power Amplifier) - GPIO53
    #define PA_ENABLE_GPIO (gpio_num_t)GPIO_NUM_53
    gpio_set_direction(PA_ENABLE_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PA_ENABLE_GPIO, 1);  // Enable PA
    ESP_LOGI("AUDIO_TEST", "PA enabled (GPIO53 set to HIGH)");
    
    // Initialize audio player (this properly configures ES8311 for playback)
    ESP_LOGI("AUDIO_TEST", "Initializing audio player...");
    esp_err_t ret = bsp_extra_player_init();
    if (ret != ESP_OK) {
        ESP_LOGE("AUDIO_TEST", "bsp_extra_player_init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI("AUDIO_TEST", "Audio player initialized successfully");
    }
    
    // Set volume to max
    bsp_extra_codec_volume_set(100, NULL);
    ESP_LOGI("AUDIO_TEST", "Volume set to 100");
    
    // Unmute if muted
    bsp_extra_codec_mute_set(false);
    ESP_LOGI("AUDIO_TEST", "Unmuted");
    
    // Wait a bit
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Generate sine wave tone for speaker test
    int16_t audio_buffer[960];
    int test_count = 0;
    int max_tests = 50;
    float frequency = 440.0f; // 440Hz tone
    float sample_rate = 16000.0f;
    float phase = 0.0f;
    float phase_increment = 2.0f * 3.14159f * frequency / sample_rate;
    
    while (test_count < max_tests) {
        // Generate sine wave
        for (int i = 0; i < 960; i++) {
            audio_buffer[i] = (int16_t)(sinf(phase) * 8000);
            phase += phase_increment;
            if (phase > 2.0f * 3.14159f) {
                phase -= 2.0f * 3.14159f;
            }
        }
        
        size_t bytes_written = 0;
        esp_err_t ret = bsp_extra_i2s_write(audio_buffer, sizeof(audio_buffer), &bytes_written, 1000);
        
        if (ret == ESP_OK) {
            ESP_LOGI("AUDIO_TEST", "Speaker wrote %d bytes", bytes_written);
        } else {
            ESP_LOGE("AUDIO_TEST", "Speaker write failed: %s", esp_err_to_name(ret));
        }
        
        test_count++;
    }
    
    ESP_LOGI("AUDIO_TEST", "=== Speaker Test Completed ===");
    vTaskDelete(NULL);
}
#endif

// BME680 sensor data (global for UI access, NOT static so ui.cpp can access)
float g_temperature = 0.0f;
float g_humidity = 0.0f;
float g_pressure = 1013.0f;

static void sensor_data_interceptor(const ble_sensor_data_t *data)
{
    // 0. Update global variables with real telemetry data from the edge sensing node
    if (data->temp > -40.0f && data->temp < 100.0f) {
        g_temperature = data->temp;
    }
    if (data->hum >= 0.0f && data->hum <= 100.0f) {
        g_humidity = data->hum;
    }

    // 1. 拦截数据，触发常规 UI 渲染
    ui_handle_sensor_data(data);
    
    // 2. 缓存进 PSRAM 用于定时 CSV 批量归档
    csv_buffer_append(data);

    // 3. 实数同步：如果是从 C6 传来的最新数据且云端上传使能，立即推送云端服务器
    if (cloud_sync_is_enabled()) {
        cloud_sync_post_c6_data(data);
    }

    // 3. 本地自动逻辑：新鲜度 <= 40 开启舵机舱门，新鲜度 > 40 关闭；仅在 P4 离线（WiFi 未连接）且自动换气使能（lid_auto）时触发
    if (!wifi_is_connected() && data->lid_auto) {
        static bool last_lid_auto_state = false;
        static bool has_last_state = false;
        bool should_open = (data->fresh <= 40);
        if (!has_last_state || last_lid_auto_state != should_open) {
            uart_receiver_send_cmd(data->node, should_open ? "lid_on" : "lid_off");
            ui_set_lid(should_open);
            last_lid_auto_state = should_open;
            has_last_state = true;
            ESP_LOGI("AUTO_LID", "P4 offline auto control triggered: fresh=%d -> sending lid %s via CMD", data->fresh, should_open ? "on" : "off");
        }
    }
}

static void console_rx_task(void *pvParameters)
{
    ESP_LOGI("CLI", "Console command listener task started (stdin ready).");
    char line[128];
    while (1) {
        // 使用 fgets 读取调试串口（stdin）输入
        if (fgets(line, sizeof(line), stdin)) {
            // 过滤末尾的换行符
            int len = strlen(line);
            while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
                line[len - 1] = '\0';
                len--;
            }
            if (len > 0) {
                ESP_LOGI("CLI", "Console raw input: '%s'", line);
                
                // 解析并校验 'model delete <ID>' 命令
                if (strncmp(line, "model delete ", 13) == 0) {
                    int model_idx = atoi(line + 13);
                    ESP_LOGI("CLI", "Executing remote S3 model delete command for index: %d", model_idx);
                    
                    // 向下位机发送串口删除指令
                    char del_cmd[64];
                    snprintf(del_cmd, sizeof(del_cmd), "model delete %d", model_idx);
                    char first_name[32] = "";
                    uart_receiver_get_first_paired_name(first_name, sizeof(first_name));
                    if (first_name[0]) {
                        uart_receiver_send_cmd(first_name, del_cmd);
                    }
                    
                    printf("CLI_ACK: Sent CMD:%s:%s\n", first_name[0] ? first_name : "(?)", del_cmd);
                } else if (strcmp(line, "model list") == 0) {
                    // 支持 model list 查询列表指令
                    ESP_LOGI("CLI", "Requesting model list from S3...");
                    char first_name[32] = "";
                    uart_receiver_get_first_paired_name(first_name, sizeof(first_name));
                    if (first_name[0]) {
                        uart_receiver_send_cmd(first_name, "model_list");
                    }
                    printf("CLI_ACK: Sent CMD:%s:model_list\n", first_name[0] ? first_name : "(?)");
                } else if (strcmp(line, "pair list") == 0) {
                    uart_receiver_print_pairings();
                    printf("CLI_ACK: Displayed pairing list.\n");
                } else if (strcmp(line, "pair clear") == 0) {
                    uart_receiver_clear_all_pairings();
                    uart_receiver_send("CANCEL ALL");
                    printf("CLI_ACK: Cleared pairing list and sent 'CANCEL ALL' to C6.\n");
                } else if (strncmp(line, "pair ", 5) == 0) {
                    char name[32] = {0};
                    char mac[32] = {0};
                    if (sscanf(line + 5, "%31s %31s", name, mac) == 2) {
                        uart_receiver_pair_device(name, mac);
                        printf("CLI_ACK: Paired device %s @ %s.\n", name, mac);
                    } else {
                        printf("CLI_ACK: Invalid format. Use 'pair <name> <MAC>'\n");
                    }
                } else if (strncmp(line, "listen ", 7) == 0) {
                    char name[32] = {0};
                    char mac[32] = {0};
                    if (sscanf(line + 7, "%31s %31s", name, mac) == 2) {
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "LISTEN:%s:%s", name, mac);
                        uart_receiver_send(cmd);
                        printf("CLI_ACK: Sent '%s' to C6 bridge.\n", cmd);
                    } else {
                        printf("CLI_ACK: Invalid format. Use 'listen <name> <MAC>'\n");
                    }
                } else if (strncmp(line, "cancel ", 7) == 0) {
                    char name[32] = {0};
                    if (sscanf(line + 7, "%31s", name) == 1) {
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "CANCEL:%s", name);
                        uart_receiver_send(cmd);
                        printf("CLI_ACK: Sent '%s' to C6 bridge.\n", cmd);
                    } else {
                        printf("CLI_ACK: Invalid format. Use 'cancel <name>'\n");
                    }
                } else if (strcmp(line, "cancel all") == 0 || strcmp(line, "cancel_all") == 0) {
                    uart_receiver_send("CANCEL ALL");
                    printf("CLI_ACK: Sent 'CANCEL ALL' to C6 bridge.\n");
                } else if (strcmp(line, "list?") == 0) {
                    uart_receiver_send("LIST?");
                    printf("CLI_ACK: Sent 'LIST?' to C6 bridge.\n");
                } else if (strcmp(line, "status") == 0) {
                    uart_receiver_send("STATUS");
                    printf("CLI_ACK: Sent 'STATUS' to C6 bridge.\n");
                } else if (line[0] == '{') {
                    cJSON *root = cJSON_Parse(line);
                    if (root) {
                        cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
                        if (cmd_item && cmd_item->valuestring) {
                            char first_name[32] = "";
                            uart_receiver_get_first_paired_name(first_name, sizeof(first_name));
                            if (strcmp(cmd_item->valuestring, "lid_on") == 0) {
                                if (first_name[0]) uart_receiver_send_cmd(first_name, "lid_on");
                                ui_set_lid(true);
                                printf("CLI_ACK: Sent lid_on to %s via CMD.\n", first_name[0] ? first_name : "(no device)");
                            } else if (strcmp(cmd_item->valuestring, "lid_off") == 0) {
                                if (first_name[0]) uart_receiver_send_cmd(first_name, "lid_off");
                                ui_set_lid(false);
                                printf("CLI_ACK: Sent lid_off to %s via CMD.\n", first_name[0] ? first_name : "(no device)");
                            }
                        }
                        cJSON_Delete(root);
                    }
                } else if (strcmp(line, "lid on") == 0 || strcmp(line, "lid_on") == 0) {
                    char first_name[32] = "";
                    uart_receiver_get_first_paired_name(first_name, sizeof(first_name));
                    if (first_name[0]) uart_receiver_send_cmd(first_name, "lid_on");
                    ui_set_lid(true);
                    printf("CLI_ACK: Sent lid_on to %s via CMD.\n", first_name[0] ? first_name : "(no device)");
                } else if (strcmp(line, "lid off") == 0 || strcmp(line, "lid_off") == 0) {
                    char first_name[32] = "";
                    uart_receiver_get_first_paired_name(first_name, sizeof(first_name));
                    if (first_name[0]) uart_receiver_send_cmd(first_name, "lid_off");
                    ui_set_lid(false);
                    printf("CLI_ACK: Sent lid_off to %s via CMD.\n", first_name[0] ? first_name : "(no device)");
                } else if (strcmp(line, "lid auto on") == 0 || strcmp(line, "lid_auto_on") == 0) {
                    char first_name[32] = "";
                    uart_receiver_get_first_paired_name(first_name, sizeof(first_name));
                    if (first_name[0]) uart_receiver_send_cmd(first_name, "lid_auto_on");
                    printf("CLI_ACK: Sent lid_auto_on to %s via CMD.\n", first_name[0] ? first_name : "(no device)");
                } else if (strcmp(line, "lid auto off") == 0 || strcmp(line, "lid_auto_off") == 0) {
                    char first_name[32] = "";
                    uart_receiver_get_first_paired_name(first_name, sizeof(first_name));
                    if (first_name[0]) uart_receiver_send_cmd(first_name, "lid_auto_off");
                    printf("CLI_ACK: Sent lid_auto_off to %s via CMD.\n", first_name[0] ? first_name : "(no device)");
                } else if (strcmp(line, "lid status") == 0) {
                    char first_name[32] = "";
                    uart_receiver_get_first_paired_name(first_name, sizeof(first_name));
                    if (first_name[0]) uart_receiver_send_cmd(first_name, "lid_status");
                    printf("CLI_ACK: Sent lid_status to %s via CMD.\n", first_name[0] ? first_name : "(no device)");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void p4_maintenance_task(void *pvParameters)
{
    ESP_LOGI(TAG, "P4 core maintenance task started running...");
    while (1) {
        // 自动判定 10 分钟边界上传 CSV
        csv_buffer_check_and_flush();
        // 判定 OTA 分片重传超时
        model_mgr_check_timeout();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// BME680 task
static void bme680_task(void *pvParameters) {
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    
    ESP_LOGI(TAG, "Initializing BME680...");
    esp_err_t ret = bme680_init(i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BME680 init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    
    while (1) {
        ret = bme680_read(&g_temperature, &g_humidity, &g_pressure);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "BME680: T=%.1fC H=%.1f%% P=%.1fhPa", 
                      g_temperature, g_humidity, g_pressure);
        } else {
            ESP_LOGW(TAG, "BME680 read failed: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(2000));  // Read every 2 seconds
    }
}

extern "C" void app_main(void)
{
    // 注册 cJSON 全局内存重定向钩子到外部 PSRAM，防内部 SRAM 被高频 JSON 解析碎片化
    cJSON_Hooks hooks = {
        .malloc_fn = [](size_t size) -> void* {
            return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        },
        .free_fn = [](void* ptr) {
            heap_caps_free(ptr);
        }
    };
    cJSON_InitHooks(&hooks);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Initialize system event group first - all modules use it */
    app_system_event_group_init();
    ESP_LOGI(TAG, "System event group initialized");

    // Note: Do NOT call app_video_main() here!
    // Camera initialization will be done in ui.cpp when needed

    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI(TAG, "SPIFFS mount successfully");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // SD card is mounted lazily on first AI history save/read

    ESP_ERROR_CHECK(bsp_extra_codec_init());

    // Initialize XiaoZhi AI Voice Service
    ESP_LOGI(TAG, "Initializing XiaoZhi AI Service...");
    xiaozhi_ai_service_init();
    xiaozhi_ai_service_set_state_callback(xiaozhi_state_callback);
    xiaozhi_ai_service_set_text_callback(xiaozhi_text_callback);

    /* Initialize PPA hardware accelerator for zero-copy video pipeline */
    esp_err_t ppa_ret = app_ppa_init();
    if (ppa_ret == ESP_OK) {
        ESP_LOGI(TAG, "PPA hardware accelerator initialized - zero-copy video pipeline active");
    } else {
        ESP_LOGW(TAG, "PPA init failed: %s - will use software fallback", esp_err_to_name(ppa_ret));
    }

#if AUDIO_TEST_ENABLED
    // Start audio test tasks
    ESP_LOGI(TAG, "Starting audio tests...");
    xTaskCreatePinnedToCore(audio_mic_test_task, "mic_test", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(audio_speaker_test_task, "speaker_test", 4096, NULL, 4, NULL, 0);
#endif

#if VOLCANO_RTC_ENABLED
    // Initialize Volcano RTC service
    ESP_LOGI(TAG, "Initializing Volcano RTC service...");
    volcano_rtc_service_init();
    volcano_rtc_service_set_state_callback(volcano_rtc_state_callback);
#endif



    // Initialize CSV buffering in external PSRAM
    ESP_LOGI(TAG, "Initializing CSV circular buffer...");
    csv_buffer_init();

    // Start background maintenance loop task
    xTaskCreatePinnedToCore(p4_maintenance_task, "p4_maint", 4096, NULL, 4, NULL, 1);

    // Initialize UART Receiver (receives sensor data from S3 nodes via C6 bridge)
    ESP_LOGI(TAG, "Initializing UART Receiver...");
    uart_receiver_init();
    uart_receiver_register_cb(sensor_data_interceptor);

    // 启动控制台串口指令监听任务，钉在 Core 1 上，确保低优先级不影响系统实时性
    xTaskCreatePinnedToCore(console_rx_task, "console_rx", 4096, NULL, 3, NULL, 1);
    uart_receiver_register_warmup_cb(ui_handle_warmup);

    // Electronic Nose Gateway initialization removed permanently

    ESP_LOGI(TAG, "Starting web dashboard...");
    web_dashboard_init();

    /* BME680 task removed to prevent I2C bus contention with UI touch/audio */

    bsp_display_cfg_t cfg = {
        .hw_cfg = {
            .hdmi_resolution = BSP_HDMI_RES_NONE,
            .dsi_bus = {
                .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
            },
        },
    };
    lv_display_t *disp = lvgl_adapter_init(&cfg);
    assert(disp != nullptr && "Failed to init LVGL adapter");
    bsp_display_backlight_on();

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    // 初始化 UI 界面
    ui_init();

    // 从 NVS 恢复已配对的 sidebar app（掉电不丢失）
    ui_restore_paired_apps();

    cloud_sync_init();

    // XiaoZhi AI is initialized above and started on-demand by PTT button press
    
    ESP_LOGI(TAG, "Electronic Nose UI initialized successfully");
    ESP_LOGI(TAG, "Core Affinity: original config (PPA disabled for debugging)");

    esp_lv_adapter_unlock();
}
