/*
 * Volcano RTC Service - Using Official SDK
 * 
 * This service uses the volc_conv_ai SDK for real-time voice conversation
 */

#include <string.h>
#include <sys/param.h>
#include <inttypes.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "bsp_board_extra.h"
#include "volcano_rtc_service.h"
#include "ui.h"
#include "volc_conv_ai.h"
#include "cJSON.h"
#include "esp_sntp.h"

static const char *TAG = "VOLCANO_RTC";

// Configuration from menuconfig
#define VOLC_INSTANCE_ID    CONFIG_VOLC_INSTANCE_ID
#define VOLC_PRODUCT_KEY   CONFIG_VOLC_PRODUCT_KEY
#define VOLC_PRODUCT_SECRET CONFIG_VOLC_PRODUCT_SECRET
#define VOLC_DEVICE_NAME   CONFIG_VOLC_DEVICE_NAME
#define VOLC_BOT_ID        CONFIG_VOLC_BOT_ID

static bool s_time_synced = false;

static void time_sync_notification(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized! Unix time: %lld", tv->tv_sec);
    s_time_synced = true;
}

static bool s_sntp_initialized = false;

static void init_time_sync(void)
{
    ESP_LOGI(TAG, "Initializing SNTP time synchronization...");
    
    // Prevent re-initialization crash
    if (s_sntp_initialized) {
        ESP_LOGI(TAG, "SNTP already initialized, skipping...");
        return;
    }
    s_sntp_initialized = true;
    
    // Configure timezone
    setenv("TZ", "GMT-8", 1);
    tzset();
    
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // Use high-reliability Chinese NTP servers as priority
    sntp_setservername(0, "ntp.aliyun.com");
    sntp_setservername(1, "time1.cloud.tencent.com");
    sntp_setservername(2, "cn.pool.ntp.org");
    sntp_setservername(3, "pool.ntp.org");
    
    // Set time sync notification
    sntp_set_time_sync_notification_cb(time_sync_notification);
    
    // Initialize SNTP
    sntp_init();
    
    // Wait for time sync with longer timeout
    int attempts = 0;
    while (!s_time_synced && attempts < 60) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
        ESP_LOGI(TAG, "Waiting for time sync... (%d/60)", attempts);
    }
    
    if (s_time_synced) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        char strftime_buf[64];
        strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        ESP_LOGI(TAG, "Current time: %s", strftime_buf);
    } else {
        ESP_LOGE(TAG, "Time sync failed, using fallback time");
        // Set fallback time to approximate current time (2026-04-01 00:00:00 UTC)
        struct timeval tv = { .tv_sec = 1775023200, .tv_usec = 0 }; 
        if (settimeofday(&tv, NULL) != 0) {
            ESP_LOGE(TAG, "Failed to set fallback time!");
        }
        s_time_synced = true;
        
        time_t now;
        time(&now);
        ESP_LOGI(TAG, "Manual time set to: %lld", (long long)now);
    }
}

// State
static volatile volcano_rtc_state_t s_state = VOLCANO_RTC_STATE_IDLE;
static volatile bool s_running = false;
static volatile bool s_should_stop = false;
static TaskHandle_t s_task_handle = NULL;
static TaskHandle_t s_audio_task_handle = NULL;
static TaskHandle_t s_speaker_task_handle = NULL;
static volcano_rtc_state_cb_t s_state_cb = NULL;
static volcano_rtc_text_cb_t s_text_cb = NULL;

// Volcano SDK handle
static volc_engine_t s_engine = NULL;

// Audio player
static bool s_player_initialized = false;

// Audio queue - ring buffer in PSRAM (original simple implementation)
#define MAX_AUDIO_PACKET_SIZE 1024
#define AUDIO_QUEUE_CAPACITY 64

static uint8_t* audio_queue = NULL;        // Ring buffer in PSRAM
static size_t* audio_queue_len = NULL;     // Length of each packet
static int audio_queue_write_idx = 0;
static int audio_queue_read_idx = 0;
static int audio_queue_count = 0;
static int audio_queue_capacity = 0;
static bool reply_complete = false;

static bool init_audio_queue(void)
{
    if (audio_queue != NULL) {
        return true;
    }
    
    audio_queue_capacity = AUDIO_QUEUE_CAPACITY;
    audio_queue = (uint8_t*)heap_caps_malloc(audio_queue_capacity * 1024, MALLOC_CAP_SPIRAM);
    audio_queue_len = (size_t*)heap_caps_malloc(audio_queue_capacity * sizeof(size_t), MALLOC_CAP_SPIRAM);
    
    if (audio_queue && audio_queue_len) {
        ESP_LOGI(TAG, "Audio queue initialized in PSRAM: %d x 1KB", audio_queue_capacity);
        return true;
    }
    
    ESP_LOGE(TAG, "Failed to allocate audio queue in PSRAM");
    return false;
}

static void free_audio_queue(void)
{
    if (audio_queue) {
        heap_caps_free(audio_queue);
        audio_queue = NULL;
    }
    if (audio_queue_len) {
        heap_caps_free(audio_queue_len);
        audio_queue_len = NULL;
    }
    audio_queue_capacity = 0;
    audio_queue_count = 0;
}

static void enable_pa(void)
{
    ESP_LOGI(TAG, "PA control: GPIO53");
}

static void disable_pa(void)
{
    ESP_LOGI(TAG, "PA disabled");
}

static void update_state(volcano_rtc_state_t new_state, const char* message)
{
    s_state = new_state;
    if (s_state_cb) {
        s_state_cb(new_state, message);
    }
    ESP_LOGI(TAG, "State: %d - %s", new_state, message ? message : "");
}

// Event callback
static void on_volc_event(volc_engine_t handle, volc_event_t* event, void* user_data)
{
    switch (event->code) {
    case VOLC_EV_CONNECTED:
        ESP_LOGI(TAG, "Volc Engine connected!");
        update_state(VOLCANO_RTC_STATE_CONNECTED, "Connected!");
        break;
    case VOLC_EV_DISCONNECTED:
        ESP_LOGI(TAG, "Volc Engine disconnected");
        update_state(VOLCANO_RTC_STATE_IDLE, "Disconnected");
        break;
    case VOLC_EV_QUOTA_EXCEEDED:
        ESP_LOGE(TAG, "Volc Engine quota exceeded!");
        update_state(VOLCANO_RTC_STATE_ERROR, "Quota exceeded");
        break;
    default:
        ESP_LOGI(TAG, "Volc Engine event: %d", event->code);
        break;
    }
}

// Audio data received callback - store in ring buffer
static void on_volc_audio_data(volc_engine_t handle, const void* data_ptr, size_t data_len, 
                               volc_audio_frame_info_t* info_ptr, void* user_data)
{
    if (data_len > 0 && data_len < 1024 && s_player_initialized && 
        audio_queue && audio_queue_len && audio_queue_count < audio_queue_capacity) {
        memcpy(audio_queue + audio_queue_write_idx * 1024, data_ptr, data_len);
        audio_queue_len[audio_queue_write_idx] = data_len;
        audio_queue_write_idx = (audio_queue_write_idx + 1) % audio_queue_capacity;
        audio_queue_count++;
    }
}

// Speaker playback task - with pre-buffer for smooth playback
static void speaker_play_task(void* pvParameters)
{
    size_t written = 0;
    int pre_buffer_count = 0;
    const int PRE_BUFFER_THRESHOLD = 10;  // Wait for 10 packets before starting
    
    while (!s_should_stop) {
        if (audio_queue_count > 0 && s_player_initialized) {
            // Pre-buffer phase: wait for enough data
            if (pre_buffer_count < PRE_BUFFER_THRESHOLD) {
                pre_buffer_count++;
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            
            // Playback phase
            bsp_extra_i2s_write(audio_queue + audio_queue_read_idx * 1024, 
                              audio_queue_len[audio_queue_read_idx], &written, 20);
            audio_queue_read_idx = (audio_queue_read_idx + 1) % audio_queue_capacity;
            audio_queue_count--;
            pre_buffer_count = 0;
        } else {
            // No data, small delay
            vTaskDelay(pdMS_TO_TICKS(2));
            pre_buffer_count = 0;
        }
    }
    vTaskDelete(NULL);
}

// Conversation status callback
static void on_volc_conversation_status(volc_engine_t handle, volc_conv_status_e status, void* user_data)
{
    switch (status) {
    case VOLC_CONV_STATUS_LISTENING:
        ESP_LOGI(TAG, "AI is listening...");
        // Clear audio queue for next reply
        audio_queue_count = 0;
        audio_queue_write_idx = 0;
        audio_queue_read_idx = 0;
        reply_complete = false;
        update_state(VOLCANO_RTC_STATE_RECORDING, "AI is listening");
        break;
    case VOLC_CONV_STATUS_THINKING:
        ESP_LOGI(TAG, "AI is thinking...");
        update_state(VOLCANO_RTC_STATE_PLAYING, "AI is thinking");
        break;
    case VOLC_CONV_STATUS_ANSWERING:
        ESP_LOGI(TAG, "AI is answering...");
        break;
    case VOLC_CONV_STATUS_INTERRUPTED:
        ESP_LOGI(TAG, "Conversation interrupted");
        audio_queue_count = 0;
        audio_queue_write_idx = 0;
        audio_queue_read_idx = 0;
        reply_complete = false;
        break;
    case VOLC_CONV_STATUS_ANSWER_FINISH:
        ESP_LOGI(TAG, "AI answer finished");
        reply_complete = true;
        update_state(VOLCANO_RTC_STATE_CONNECTED, "Ready");
        break;
    default:
        break;
    }
}

// Message received callback (subtitles)
static void on_volc_message_data(volc_engine_t handle, const void* message, size_t size, 
                                  volc_message_info_t* info_ptr, void* user_data)
{
    if (size > 8 && size < 4096) {
        char* msg_buffer = (char*)malloc(size + 2);
        if (msg_buffer) {
            memcpy(msg_buffer, message, size);
            msg_buffer[size] = 0;
            
            // Check magic bytes
            if (msg_buffer[0] == 's' && msg_buffer[1] == 'u' && 
                msg_buffer[2] == 'b' && msg_buffer[3] == 'v') {
                cJSON* root = cJSON_Parse(msg_buffer + 8);
                if (root) {
                    cJSON* type_obj = cJSON_GetObjectItem(root, "type");
                    if (type_obj && strcmp("subtitle", cJSON_GetStringValue(type_obj)) == 0) {
                        cJSON* data_obj_arr = cJSON_GetObjectItem(root, "data");
                        cJSON* obj = NULL;
                        cJSON_ArrayForEach(obj, data_obj_arr) {
                            cJSON* text_obj = cJSON_GetObjectItem(obj, "text");
                            cJSON* definite_obj = cJSON_GetObjectItem(obj, "definite");
                            if (text_obj && definite_obj) {
                                char* text = cJSON_GetStringValue(text_obj);
                                ESP_LOGI(TAG, "AI said: %s", text);
                                if (s_text_cb) {
                                    s_text_cb(text);
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
            free(msg_buffer);
        }
    }
}

// Audio capture and send task
static void audio_capture_task(void* pvParameters)
{
    ESP_LOGI(TAG, "Audio capture task started");
    
    // Use smaller buffer to reduce memory pressure
    int16_t audio_buffer[256];
    size_t bytes_read = 0;
    
    // Only set codec format once at the beginning with error handling
    // Add guard to prevent multiple init
    static bool codec_initialized = false;
    if (!codec_initialized) {
        esp_err_t init_ret = bsp_extra_codec_set_fs(16000, 16, I2S_SLOT_MODE_MONO);
        if (init_ret == ESP_OK) {
            codec_initialized = true;
            ESP_LOGI(TAG, "Codec initialized for recording");
        } else {
            ESP_LOGW(TAG, "Codec init returned: %d, will use default", init_ret);
        }
        // Add delay to let codec stabilize
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    
    int error_count = 0;
    while (!s_should_stop && s_engine && error_count < 10) {
        // Only read if engine is still connected
        if (s_state != VOLCANO_RTC_STATE_CONNECTED) {
            ESP_LOGD(TAG, "Not connected, waiting...");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // Read audio from microphone using I2S - use shorter timeout for smoother capture
        esp_err_t ret = bsp_extra_i2s_read(audio_buffer, sizeof(audio_buffer), &bytes_read, 10);
        if (ret == ESP_OK && bytes_read > 0) {
            volc_audio_frame_info_t info = {
                .data_type = VOLC_AUDIO_DATA_TYPE_PCM,
                .commit = false
            };
            int result = volc_send_audio_data(s_engine, audio_buffer, bytes_read, &info);
            if (result != 0) {
                error_count++;
                ESP_LOGW(TAG, "Send audio failed: %d (count: %d), state=%d", result, error_count, s_state);
            } else {
                error_count = 0;
            }
        } else if (ret != ESP_OK) {
            // Log read errors
            ESP_LOGV(TAG, "I2S read error: %d", ret);
        }
        
        // Minimal delay to prevent CPU hogging, but keep audio smooth
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    
    ESP_LOGI(TAG, "Audio capture task stopped");
    vTaskDelete(NULL);
}

// Main RTC task
static void volcano_rtc_task(void* pvParameters)
{
    ESP_LOGI(TAG, "Volcano RTC task started");
    
    // Wait for WiFi/Network to be ready before NTP sync
    ESP_LOGI(TAG, "Waiting for network connectivity...");
    int wifi_wait_attempts = 0;
    while (wifi_wait_attempts < 30) {
        esp_netif_ip_info_t ip_info;
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            if (ip_info.ip.addr != 0) {
                ESP_LOGI(TAG, "Network ready (IP: " IPSTR ")", IP2STR(&ip_info.ip));
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        wifi_wait_attempts++;
    }
    
    // Additional delay for network stability (RTC needs stable network)
    ESP_LOGI(TAG, "Waiting for network stability...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Initialize time synchronization first
    init_time_sync();
    
    enable_pa();
    
    // Initialize audio player
    bsp_extra_player_init();
    bsp_extra_codec_volume_set(ui_get_current_volume(), NULL);
    bsp_extra_codec_mute_set(false);
    
    // Initialize audio queue in PSRAM
    if (!init_audio_queue()) {
        ESP_LOGE(TAG, "Failed to init audio queue");
        disable_pa();
        vTaskDelete(NULL);
        return;
    }
    
    s_player_initialized = true;
    
    // Create configuration JSON (WebSocket mode)
    char config_buf[2048];
    snprintf(config_buf, sizeof(config_buf),
        "{"
        "\"ver\": 1,"
        "\"iot\": {"
        "  \"instance_id\": \"%s\","
        "  \"product_key\": \"%s\","
        "  \"product_secret\": \"%s\","
        "  \"device_name\": \"%s\""
        "},"
        "\"ws\": {"
        "  \"log_level\": 3,"
        "  \"params\": ["
        "    \"{\\\"debug\\\":{\\\"log_to_console\\\":1}}\","
        "    \"{\\\"audio\\\":{\\\"codec\\\":{\\\"pcma\\\":{\\\"s_samples_per_frame\\\":480}}}}\","
        "    \"{\\\"network\\\":{\\\"timeout_ms\\\":8000}}\""
        "  ]"
        "},"
        "\"audio\": {"
        "  \"codec\": 4"
        "}"
        "}",
        VOLC_INSTANCE_ID,
        VOLC_PRODUCT_KEY,
        VOLC_PRODUCT_SECRET,
        VOLC_DEVICE_NAME);
    
    ESP_LOGI(TAG, "Creating Volcano Engine...");
    
    // Setup event handlers
    volc_event_handler_t event_handler = {
        .on_volc_event = on_volc_event,
        .on_volc_conversation_status = on_volc_conversation_status,
        .on_volc_audio_data = on_volc_audio_data,
        .on_volc_message_data = on_volc_message_data,
    };
    
    // Create engine
    int error = volc_create(&s_engine, config_buf, &event_handler, NULL);
    if (error != 0 || s_engine == NULL) {
        ESP_LOGE(TAG, "Failed to create Volcano Engine: %d", error);
        update_state(VOLCANO_RTC_STATE_ERROR, "Create failed");
        disable_pa();
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Volcano Engine created successfully");
    
    // Start conversation
    update_state(VOLCANO_RTC_STATE_CONNECTING, "Connecting...");
    
    volc_opt_t opt = {
        .mode = VOLC_MODE_WS,
        .bot_id = VOLC_BOT_ID,
        .params = "{}"
    };
    
    error = volc_start(s_engine, &opt);
    if (error != 0) {
        ESP_LOGE(TAG, "Failed to start Volcano Engine: %d", error);
        update_state(VOLCANO_RTC_STATE_ERROR, "Start failed");
        volc_destroy(s_engine);
        s_engine = NULL;
        disable_pa();
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Volcano Engine started, waiting for connection...");
    
    // Wait for connection state - increase timeout
    int wait_count = 0;
    while (s_state != VOLCANO_RTC_STATE_CONNECTED && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }
    
    // Always start audio capture task when connected (even if delayed)
    if (s_state == VOLCANO_RTC_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Connected! Starting audio capture...");
        
        // 麦克风任务: 优先级10, Core 0
        xTaskCreatePinnedToCore(
            audio_capture_task,
            "audio_capture",
            12288,  // 增大到12KB
            NULL,
            10,
            &s_audio_task_handle,
            0
        );
        ESP_LOGI(TAG, "Audio capture task created (priority 10, core 0, stack 12KB)");
        
        // 扬声器播放任务: 最高优先级20确保音频流畅, Core 1专用
        xTaskCreatePinnedToCore(
            speaker_play_task,
            "speaker_play",
            8192,  // 增大到8KB
            NULL,
            20,
            &s_speaker_task_handle,
            1  // Core 1: 专用于音频播放,避免被SDK占用
        );
        ESP_LOGI(TAG, "Speaker playback task created (priority 20, core 1, stack 8KB)");
    } else {
        ESP_LOGW(TAG, "Failed to connect, state=%d", s_state);
    }
    
    // Main loop - monitor connection state and auto-reconnect
    int reconnect_count = 0;
    while (!s_should_stop && s_engine) {
        if (s_state != VOLCANO_RTC_STATE_CONNECTED) {
            reconnect_count++;
            ESP_LOGI(TAG, "Not connected (state=%d), reconnect attempt %d", s_state, reconnect_count);
            // 增加重连次数上限到30次 (约30秒)
            if (reconnect_count > 30) {
                ESP_LOGW(TAG, "Too many reconnections (%d), stopping...", reconnect_count);
                break;
            }
            ESP_LOGI(TAG, "Waiting for reconnection...");
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            reconnect_count = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "Stopping Volcano Engine...");
    
    // Signal audio capture to stop FIRST - this must happen before anything else
    s_should_stop = true;
    ESP_LOGI(TAG, "Signaled audio task to stop");
    
    // Wait for audio task to exit gracefully (no force delete!)
    if (s_audio_task_handle) {
        ESP_LOGI(TAG, "Waiting for audio task to stop...");
        for (int i = 0; i < 100; i++) {  // Wait up to 5 seconds
            vTaskDelay(pdMS_TO_TICKS(50));
            if (s_audio_task_handle == NULL) {
                ESP_LOGI(TAG, "Audio task stopped gracefully");
                break;
            }
        }
        // Don't force delete - let the task finish naturally
        s_audio_task_handle = NULL;
    }
    
    // Free audio queue
    free_audio_queue();
    
    // Stop engine
    if (s_engine) {
        ESP_LOGI(TAG, "Stopping volc engine...");
        volc_stop(s_engine);
        volc_destroy(s_engine);
        s_engine = NULL;
    }
    
    s_player_initialized = false;
    disable_pa();
    
    ESP_LOGI(TAG, "Volcano RTC task stopped");
    update_state(VOLCANO_RTC_STATE_IDLE, "Stopped");
    vTaskDelete(NULL);
}

esp_err_t volcano_rtc_service_init(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Volcano RTC Service - Initializing");
    ESP_LOGI(TAG, "Instance: %s", VOLC_INSTANCE_ID);
    ESP_LOGI(TAG, "Device: %s", VOLC_DEVICE_NAME);
    ESP_LOGI(TAG, "Bot ID: %s", VOLC_BOT_ID);
    ESP_LOGI(TAG, "===========================================");
    
    return ESP_OK;
}

esp_err_t volcano_rtc_service_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Service already running");
        return ESP_OK;
    }
    
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi not connected!");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "WiFi connected to: %s", ap_info.ssid);
    
    s_running = true;
    s_should_stop = false;
    update_state(VOLCANO_RTC_STATE_CONNECTING, "Starting...");
    
    BaseType_t ret = xTaskCreatePinnedToCore(
        volcano_rtc_task,
        "volcano_rtc",
        16384,  // Increased from 8192 to 16KB for TLS/websocket stability
        NULL,
        5,
        &s_task_handle,
        0  // Core 0: 留给网络和SDK处理
    );
    
    if (ret != pdPASS) {
        s_running = false;
        update_state(VOLCANO_RTC_STATE_ERROR, "Task creation failed");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t volcano_rtc_service_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping Volcano RTC service...");
    s_should_stop = true;
    s_running = false;
    
    s_task_handle = NULL;
    
    update_state(VOLCANO_RTC_STATE_IDLE, "Stopped");
    return ESP_OK;
}

esp_err_t volcano_rtc_service_set_state_callback(volcano_rtc_state_cb_t callback)
{
    s_state_cb = callback;
    return ESP_OK;
}

esp_err_t volcano_rtc_service_set_text_callback(volcano_rtc_text_cb_t callback)
{
    s_text_cb = callback;
    return ESP_OK;
}

volcano_rtc_state_t volcano_rtc_service_get_state(void)
{
    return s_state;
}

bool volcano_rtc_service_is_connected(void)
{
    return s_state == VOLCANO_RTC_STATE_CONNECTED ||
           s_state == VOLCANO_RTC_STATE_RECORDING ||
           s_state == VOLCANO_RTC_STATE_PLAYING;
}
