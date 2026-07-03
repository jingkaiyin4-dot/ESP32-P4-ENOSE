#include <string.h>
#include <sys/param.h>
#include <inttypes.h>
#include <stdint.h>
#include <math.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "opus.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "bsp_board_extra.h"
#include "bsp/esp-bsp.h"
#include "xiaozhi_ai_service.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "app_system.h"
#include "opus.h"
#include "ui.h"

static const char *TAG = "XIAOZHI_AI";

// XiaoZhi V2 Protocol Header (Big Endian)
typedef struct {
    uint16_t version;       // version (2)
    uint16_t type;          // 0: Audio, 1: JSON
    uint32_t reserved;      // 0
    uint32_t timestamp;     // ms
    uint32_t payload_size;  // data size
} __attribute__((packed)) xiaozhi_v2_header_t;

// P4 Binary Protocol Audio Headers (V3.0)
typedef struct {
    uint8_t magic[4];       // "P4AS"
    uint8_t type;           // 0x01 = Opus audio
    uint16_t seq;           // sequence number (big-endian)
    uint16_t length;        // audio length (big-endian)
} __attribute__((packed)) p4_audio_header_t;

typedef struct {
    uint8_t magic[4];       // "SRTT"
    uint8_t type;           // 0x02 = Opus TTS audio
    uint16_t seq;           // sequence number (big-endian)
    uint16_t length;        // audio length (big-endian)
} __attribute__((packed)) srtt_audio_header_t;


// State and context
static volatile xiaozhi_state_t s_state = XIAOZHI_STATE_IDLE;
static volatile bool s_running = false;
static volatile bool s_should_stop = false;
static volatile bool s_http_tts_mode = false;
static volatile bool s_server_hello_received = false;
static volatile bool s_activation_pending = false;
static char s_session_id[64] = {0};
static esp_websocket_client_handle_t s_ws_client = NULL;
static xiaozhi_state_cb_t s_state_cb = NULL;
static xiaozhi_text_cb_t s_text_cb = NULL;

volatile int g_training_progress = -1;
volatile int g_training_cur_epoch = 0;
volatile int g_training_tot_epoch = 0;

static int s_server_sample_rate = 16000;
static SemaphoreHandle_t s_audio_mutex = NULL;
static volatile uint32_t s_last_packet_time = 0;
void xiaozhi_ai_service_test_speaker(void);
static void clear_ringbuffers(void);
static RingbufHandle_t s_play_ring_buf = NULL;
static RingbufHandle_t s_opus_ring_buf = NULL;
static uint8_t *s_ws_rx_assembly_buf = NULL;
static size_t s_ws_rx_assembly_len = 0;
static size_t s_ws_rx_assembly_alloc_sz = 0;
static uint8_t *s_opus_assembly_buf = NULL;
static size_t s_opus_assembly_len = 0;
static size_t s_opus_assembly_alloc_sz = 0;
static uint32_t s_opus_assembly_timestamp = 0;
static TaskHandle_t s_task_handle = NULL;
static TaskHandle_t s_audio_task_handle = NULL;
static TaskHandle_t s_decode_task_handle = NULL;
static TaskHandle_t s_speaker_task_handle = NULL;

// 动态 PSRAM 任务栈（栈在外部 PSRAM）与 静态内部 SRAM 任务控制块（TCB 必须在内部以过 FreeRTOS 安全校验）
static StackType_t *s_main_task_stack = NULL;
static StaticTask_t s_main_task_tcb;
static StackType_t *s_audio_task_stack = NULL;
static StaticTask_t s_audio_task_tcb;
static StackType_t *s_decode_task_stack = NULL;
static StaticTask_t s_decode_task_tcb;
static StackType_t *s_speaker_task_stack = NULL;
static StaticTask_t s_speaker_task_tcb;
static OpusDecoder *s_opus_decoder = NULL;
static OpusEncoder *s_opus_encoder = NULL;
static int16_t *s_opus_decode_buf = NULL;
static uint8_t *s_mic_v2_buf = NULL;   // Static PSRAM buffer for Mic V2 frames
static bool s_ptt_active = false;      // PTT (Press-to-Talk) state
static bool s_listen_session_active = false;
static bool s_restore_capture_pending = false;
static volatile bool s_tts_stream_active = false;
static volatile bool s_waiting_for_abort_ack = false;
static uint64_t s_abort_sent_time = 0;
static uint64_t s_tts_stop_time = 0;


#define OPUS_RINGBUF_SIZE (64 * 1024)
#define PCM_RINGBUF_SIZE (128 * 1024)
#define WS_BUFFER_SIZE (8192)           // 8KB receive buffer (balanced for ESP32-P4 internal RAM safety)

// Audio parameters
#define SAMPLE_RATE 16000
#define FRAME_SIZE 320     // 20ms @ 16kHz
#define DECODED_PCM_BUFFER_SIZE (64 * 1024) // 64KB in Internal RAM for DMA safety

static int s_decoder_sample_rate = SAMPLE_RATE;
static int s_codec_sample_rate = SAMPLE_RATE;

static esp_err_t set_codec_sample_rate(int sample_rate) {
    if (sample_rate <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    if (s_audio_mutex && xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ret = bsp_extra_codec_set_fs(sample_rate, 16, I2S_SLOT_MODE_STEREO);
        if (ret == ESP_OK) {
            s_codec_sample_rate = sample_rate;
            ESP_LOGI(TAG, "Codec sample rate set to %d Hz", sample_rate);
        } else {
            ESP_LOGE(TAG, "Failed to set codec sample rate to %d Hz: %s", sample_rate, esp_err_to_name(ret));
        }
        xSemaphoreGive(s_audio_mutex);
    } else {
        ESP_LOGE(TAG, "Failed to take audio mutex to set codec sample rate");
        ret = ESP_ERR_TIMEOUT;
    }
    return ret;
}

static esp_err_t recreate_decoder_if_needed(int sample_rate) {
    if (sample_rate <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_opus_decoder != NULL && s_decoder_sample_rate == sample_rate) {
        return ESP_OK;
    }

    if (s_opus_decoder != NULL) {
        opus_decoder_destroy(s_opus_decoder);
        s_opus_decoder = NULL;
    }

    int err = OPUS_OK;
    s_opus_decoder = opus_decoder_create(sample_rate, 1, &err);
    if (err != OPUS_OK || s_opus_decoder == NULL) {
        ESP_LOGE(TAG, "Failed create Opus decoder at %d Hz: %d", sample_rate, err);
        return ESP_FAIL;
    }

    s_decoder_sample_rate = sample_rate;
    ESP_LOGI(TAG, "Opus decoder created (%d Hz mono)", sample_rate);
    return ESP_OK;
}

static esp_err_t prepare_tts_output(int sample_rate) {
    if (sample_rate <= 0) {
        sample_rate = SAMPLE_RATE;
    }

    esp_err_t ret = recreate_decoder_if_needed(sample_rate);
    if (ret != ESP_OK) {
        return ret;
    }
    return set_codec_sample_rate(sample_rate);
}

static esp_err_t restore_capture_output(void) {
    return set_codec_sample_rate(SAMPLE_RATE);
}

static void update_state(xiaozhi_state_t new_state, const char* message) {
    s_state = new_state;
    if (s_state_cb) {
        s_state_cb(new_state, message);
    }
    if (message) {
        ESP_LOGI(TAG, "State -> %d: %s", (int)new_state, message);
    } else {
        ESP_LOGI(TAG, "State -> %d", (int)new_state);
    }
}


#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"

static void send_json_msg(const char* json_str) {
    if (!s_ws_client) {
        ESP_LOGW(TAG, "TX JSON skipped: websocket client is NULL");
        return;
    }
    if (!esp_websocket_client_is_connected(s_ws_client)) {
        ESP_LOGW(TAG, "TX JSON skipped: websocket not connected, payload=%s", json_str);
        return;
    }
    ESP_LOGI(TAG, "TX JSON: %s", json_str);
    esp_websocket_client_send_text(s_ws_client, json_str, strlen(json_str), portMAX_DELAY);
}

static void handle_server_msg(const char* json_str) {
    ESP_LOGI(TAG, "Server Msg (TEXT): %s", json_str);
    
    if (!s_server_hello_received) {
        cJSON *root = cJSON_Parse(json_str);
        if (root) {
            cJSON *type_item = cJSON_GetObjectItem(root, "type");
            cJSON *transport_item = cJSON_GetObjectItem(root, "transport");
            if (type_item && cJSON_IsString(type_item) && 
                strcmp(type_item->valuestring, "hello") == 0 &&
                transport_item && cJSON_IsString(transport_item) &&
                strcmp(transport_item->valuestring, "websocket") == 0) {
                s_server_hello_received = true;
                
                cJSON *session_item = cJSON_GetObjectItem(root, "session_id");
                if (session_item && cJSON_IsString(session_item)) {
                    strncpy(s_session_id, session_item->valuestring, sizeof(s_session_id) - 1);
                    ESP_LOGI(TAG, "Session ID: %s", s_session_id);
                }
                
                cJSON *audio_params = cJSON_GetObjectItem(root, "audio_params");
                if (audio_params) {
                    cJSON *server_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
                    if (server_rate && cJSON_IsNumber(server_rate)) {
                        s_server_sample_rate = server_rate->valueint;
                        if (recreate_decoder_if_needed(s_server_sample_rate) != ESP_OK) {
                            ESP_LOGW(TAG, "Failed to prime decoder for %d Hz", s_server_sample_rate);
                        }
                        ESP_LOGI(TAG, "Server sample rate: %d", s_server_sample_rate);
                    }
                }
                
                ESP_LOGI(TAG, "Server hello received, session ready.");
                update_state(XIAOZHI_STATE_CONNECTED, "Ready");
            }
            cJSON_Delete(root);
        }
    } else {
        cJSON *root = cJSON_Parse(json_str);
        if (root) {
            cJSON *type_item = cJSON_GetObjectItem(root, "type");
            if (type_item && cJSON_IsString(type_item)) {
                if (strcmp(type_item->valuestring, "state") == 0 || strcmp(type_item->valuestring, "status") == 0) {
                    cJSON *state_item = cJSON_GetObjectItem(root, "state");
                    if (!state_item) {
                        state_item = cJSON_GetObjectItem(root, "status");
                    }
                    if (state_item && cJSON_IsString(state_item)) {
                        if (strcmp(state_item->valuestring, "listening") == 0) {
                            if (s_waiting_for_abort_ack) {
                                ESP_LOGI(TAG, "Abort ACK received: status=listening");
                                s_waiting_for_abort_ack = false;
                                if (s_ptt_active && !s_listen_session_active) {
                                    clear_ringbuffers();
                                    s_listen_session_active = true;
                                    ESP_LOGI(TAG, "PTT (after abort): Start Listening (manual)...");
                                    char json[128];
                                    snprintf(json, sizeof(json), "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}", s_session_id);
                                    send_json_msg(json);
                                    update_state(XIAOZHI_STATE_LISTENING, "Listening...");
                                } else {
                                    update_state(XIAOZHI_STATE_CONNECTED, "Ready");
                                }
                            } else {
                                if (s_ptt_active) {
                                    update_state(XIAOZHI_STATE_LISTENING, NULL);
                                } else {
                                    update_state(XIAOZHI_STATE_CONNECTED, "Ready");
                                }
                            }
                        }
                        else if (strcmp(state_item->valuestring, "thinking") == 0) update_state(XIAOZHI_STATE_THINKING, NULL);
                        else if (strcmp(state_item->valuestring, "speaking") == 0) update_state(XIAOZHI_STATE_SPEAKING, NULL);
                        else if (strcmp(state_item->valuestring, "idle") == 0) update_state(XIAOZHI_STATE_CONNECTED, NULL);
                    }
                } else if (strcmp(type_item->valuestring, "tts") == 0) {
                    cJSON *state_item = cJSON_GetObjectItem(root, "state");
                    if (state_item && cJSON_IsString(state_item)) {
                        if (strcmp(state_item->valuestring, "start") == 0) {
                            cJSON *rate_item = cJSON_GetObjectItem(root, "sample_rate");
                            int rate = rate_item ? rate_item->valueint : s_server_sample_rate;
                            s_server_sample_rate = rate;
                            s_restore_capture_pending = false;
                            s_tts_stream_active = true;
                            s_last_packet_time = esp_timer_get_time() / 1000;
                            if (prepare_tts_output(rate) != ESP_OK) {
                                ESP_LOGW(TAG, "Failed to prepare TTS output for %d Hz", rate);
                            }
                            ESP_LOGI(TAG, "TTS START: rate=%d Hz", rate);
                            update_state(XIAOZHI_STATE_SPEAKING, "Speaking");
                        } else if (strcmp(state_item->valuestring, "sentence_start") == 0) {
                            ESP_LOGI(TAG, "TTS SENTENCE START");
                            update_state(XIAOZHI_STATE_SPEAKING, "Speaking");
                        } else if (strcmp(state_item->valuestring, "stop") == 0) {
                            s_tts_stream_active = false;
                            s_restore_capture_pending = true;
                            s_tts_stop_time = esp_timer_get_time();
                            ESP_LOGI(TAG, "TTS STOP (capture restore pending)");
                            update_state(XIAOZHI_STATE_CONNECTED, "TTS Done");
                        }
                    }
                } else if (strcmp(type_item->valuestring, "text") == 0) {
                    cJSON *text_item = cJSON_GetObjectItem(root, "text");
                    if (text_item && s_text_cb) s_text_cb(text_item->valuestring);
                } else if (strcmp(type_item->valuestring, "response") == 0) {
                    cJSON *data_item = cJSON_GetObjectItem(root, "data");
                    if (data_item) {
                        cJSON *llm_reply = cJSON_GetObjectItem(data_item, "llm_reply");
                        if (llm_reply && cJSON_IsString(llm_reply)) {
                            if (s_text_cb) s_text_cb(llm_reply->valuestring);
                        } else {
                            cJSON *asr_text = cJSON_GetObjectItem(data_item, "asr_text");
                            if (asr_text && cJSON_IsString(asr_text)) {
                                if (s_text_cb) s_text_cb(asr_text->valuestring);
                            }
                        }
                    }
                } else if (strcmp(type_item->valuestring, "command") == 0) {
                    cJSON *data_obj = cJSON_GetObjectItem(root, "data");
                    if (data_obj) {
                        cJSON *action_item = cJSON_GetObjectItem(data_obj, "action");
                        if (action_item && cJSON_IsString(action_item)) {
                            if (strcmp(action_item->valuestring, "model_update") == 0) {
                                cJSON *model_name = cJSON_GetObjectItem(data_obj, "model_name");
                                cJSON *version = cJSON_GetObjectItem(data_obj, "version");
                                cJSON *download_url = cJSON_GetObjectItem(data_obj, "download_url");
                                cJSON *norm_url = cJSON_GetObjectItem(data_obj, "norm_url");
                                const char *norm_url_str = (norm_url && norm_url->valuestring) ? norm_url->valuestring : "";
                                if (model_name && version && download_url) {
                                    ESP_LOGI(TAG, "WS Push: model_update. Name: %s, Ver: %s, URL: %s, Norm URL: %s",
                                             model_name->valuestring, version->valuestring, download_url->valuestring, norm_url_str);
                                    
                                    // Send accepted response back to WS server
                                    cJSON *resp_root = cJSON_CreateObject();
                                    cJSON_AddStringToObject(resp_root, "msg_id", "push_resp");
                                    cJSON_AddStringToObject(resp_root, "type", "response");
                                    cJSON_AddNumberToObject(resp_root, "timestamp", (double)(esp_timer_get_time() / 1000));
                                    cJSON *resp_data = cJSON_CreateObject();
                                    cJSON_AddStringToObject(resp_data, "status", "accepted");
                                    cJSON_AddStringToObject(resp_data, "message", "P4 received push and starts download");
                                    cJSON_AddItemToObject(resp_root, "data", resp_data);
                                    char *resp_payload = cJSON_PrintUnformatted(resp_root);
                                    if (resp_payload) {
                                        send_json_msg(resp_payload);
                                        free(resp_payload);
                                    }
                                    cJSON_Delete(resp_root);

                                    // Trigger background model OTA download and updates
                                    extern void model_mgr_trigger_update(const char *url, const char *norm_url, const char *name, const char *version, const char *cmd_id);
                                    model_mgr_trigger_update(download_url->valuestring, norm_url_str, model_name->valuestring, version->valuestring, "");
                                }
                            }
                        }
                    }
                } else if (strcmp(type_item->valuestring, "event") == 0) {
                    cJSON *data_obj = cJSON_GetObjectItem(root, "data");
                    if (data_obj) {
                        cJSON *event_item = cJSON_GetObjectItem(data_obj, "event");
                        if (event_item && cJSON_IsString(event_item)) {
                            if (strcmp(event_item->valuestring, "training_progress") == 0) {
                                cJSON *progress = cJSON_GetObjectItem(data_obj, "progress");
                                cJSON *current_epoch = cJSON_GetObjectItem(data_obj, "current_epoch");
                                cJSON *total_epochs = cJSON_GetObjectItem(data_obj, "total_epochs");
                                int prog_val = 0;
                                if (progress) {
                                    if (cJSON_IsNumber(progress)) {
                                        prog_val = progress->valueint;
                                    } else if (cJSON_IsString(progress)) {
                                        prog_val = atoi(progress->valuestring);
                                    }
                                }
                                int cur_ep = 0;
                                if (current_epoch) {
                                    if (cJSON_IsNumber(current_epoch)) {
                                        cur_ep = current_epoch->valueint;
                                    } else if (cJSON_IsString(current_epoch)) {
                                        cur_ep = atoi(current_epoch->valuestring);
                                    }
                                }
                                int tot_ep = 0;
                                if (total_epochs) {
                                    if (cJSON_IsNumber(total_epochs)) {
                                        tot_ep = total_epochs->valueint;
                                    } else if (cJSON_IsString(total_epochs)) {
                                        tot_ep = atoi(total_epochs->valuestring);
                                    }
                                }
                                
                                if (tot_ep > 0) {
                                    int calc_prog = (cur_ep * 100) / tot_ep;
                                    if (calc_prog > prog_val) {
                                        prog_val = calc_prog;
                                    }
                                }
                                if (prog_val < 0) prog_val = 0;
                                if (prog_val > 100) prog_val = 100;
                                
                                g_training_progress = prog_val;
                                g_training_cur_epoch = cur_ep;
                                g_training_tot_epoch = tot_ep;
                                
                                char buf[128];
                                snprintf(buf, sizeof(buf), "Training Progress: %d%% (Epoch %d/%d)", prog_val, cur_ep, tot_ep);
                                extern void ui_show_ai_analysis(const char* result);
                                ui_show_ai_analysis(buf);
                                ESP_LOGI(TAG, "Training Progress: %d%% (Epoch %d/%d)", prog_val, cur_ep, tot_ep);
                            } else if (strcmp(event_item->valuestring, "training_complete") == 0) {
                                cJSON *status = cJSON_GetObjectItem(data_obj, "status");
                                cJSON *model_name = cJSON_GetObjectItem(data_obj, "model_name");
                                cJSON *download_url = cJSON_GetObjectItem(data_obj, "download_url");
                                cJSON *norm_url = cJSON_GetObjectItem(data_obj, "norm_url");
                                const char *norm_url_str = (norm_url && norm_url->valuestring) ? norm_url->valuestring : "";
                                cJSON *accuracy = cJSON_GetObjectItem(data_obj, "final_accuracy");
                                double acc_val = accuracy ? accuracy->valuedouble : 0.0;

                                g_training_progress = -1;
                                g_training_cur_epoch = 0;
                                g_training_tot_epoch = 0;

                                if (status && strcmp(status->valuestring, "success") == 0 && model_name && download_url) {
                                    char buf[256];
                                    snprintf(buf, sizeof(buf), "Training Complete! Success!\nModel: %s\nAccuracy: %.1f%%\nDownloading model to SD card...",
                                             model_name->valuestring, acc_val * 100.0);
                                    extern void ui_show_ai_analysis(const char* result);
                                    ui_show_ai_analysis(buf);
                                    ESP_LOGI(TAG, "Training Complete! Succeeded. Triggering download of %s...", model_name->valuestring);
                                    
                                    // Trigger auto-download
                                    extern void model_mgr_trigger_update(const char *url, const char *norm_url, const char *name, const char *version, const char *cmd_id);
                                    model_mgr_trigger_update(download_url->valuestring, norm_url_str, model_name->valuestring, "1.0.0", "");
                                } else {
                                    extern void ui_show_ai_analysis(const char* result);
                                    ui_show_ai_analysis("Training Failed!");
                                    ESP_LOGE(TAG, "Training Completed with failure status");
                                }
                            }
                        }
                    }
                }
            }
            cJSON_Delete(root);
        }
    }
}

// Manual UDP DNS resolution (ESP-Hosted getaddrinfo is broken, returns error 202)
static bool manual_dns_resolve(const char* hostname, char* ip_out, size_t ip_out_len) {
    ESP_LOGI(TAG, "manual_dns_resolve: resolving %s via UDP", hostname);

    const char *dns_servers[] = {"114.114.114.114", "8.8.8.8", "223.5.5.5", NULL};

    // Build DNS query
    uint8_t query[256];
    uint16_t tx_id = 0x5678;
    int qlen = 12;
    query[0] = (tx_id >> 8) & 0xFF; query[1] = tx_id & 0xFF;
    query[2] = 0x01; query[3] = 0x00;
    memset(&query[4], 0, 8);

    const char *p = hostname;
    while (*p) {
        const char *dot = strchr(p, '.');
        int seg_len = dot ? (int)(dot - p) : (int)strlen(p);
        if (seg_len > 63 || qlen + seg_len + 1 > sizeof(query) - 4) return false;
        query[qlen++] = (uint8_t)seg_len;
        memcpy(&query[qlen], p, seg_len);
        qlen += seg_len;
        p += seg_len;
        if (*p == '.') p++;
    }
    query[qlen++] = 0;
    query[qlen++] = 0; query[qlen++] = 1;
    query[qlen++] = 0; query[qlen++] = 1;

    for (int d = 0; dns_servers[d]; d++) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) continue;

        struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in dest = {
            .sin_family = AF_INET,
            .sin_port = htons(53),
            .sin_addr.s_addr = inet_addr(dns_servers[d]),
        };

        ssize_t sent = sendto(sock, query, qlen, 0, (struct sockaddr*)&dest, sizeof(dest));
        if (sent < 0) { close(sock); continue; }
        ESP_LOGI(TAG, "DNS: sent %d bytes to %s", (int)sent, dns_servers[d]);

        uint8_t resp[512];
        ssize_t rlen = recvfrom(sock, resp, sizeof(resp), 0, NULL, NULL);
        close(sock);

        if (rlen < 12) continue;

        int rpos = 12;
        while (rpos < rlen && resp[rpos] != 0) {
            if (resp[rpos] >= 0xC0) { rpos += 2; break; }
            rpos += resp[rpos] + 1;
        }
        if (rpos < rlen && resp[rpos] == 0) rpos++;
        rpos += 4;

        while (rpos + 12 <= rlen) {
            if (resp[rpos] >= 0xC0) { rpos += 2; }
            else { while (rpos < rlen && resp[rpos] != 0) rpos += resp[rpos] + 1; if (rpos < rlen) rpos++; }

            if (rpos + 10 > rlen) break;
            uint16_t rtype = (resp[rpos] << 8) | resp[rpos+1];
            rpos += 8;
            uint16_t rdlen = (resp[rpos] << 8) | resp[rpos+1];
            rpos += 2;

            if (rtype == 1 && rdlen == 4 && rpos + 4 <= rlen) {
                snprintf(ip_out, ip_out_len, "%u.%u.%u.%u",
                         resp[rpos], resp[rpos+1], resp[rpos+2], resp[rpos+3]);
                ESP_LOGI(TAG, "DNS: %s -> %s (via %s)", hostname, ip_out, dns_servers[d]);
                return true;
            }
            rpos += rdlen;
        }
    }
    ESP_LOGE(TAG, "DNS: failed to resolve %s", hostname);
    return false;
}

// Resolve hostname to IP, skipping broken getaddrinfo on ESP-Hosted
static bool resolve_host_to_ip(const char* hostname, char* ip_out, size_t ip_out_len) {
    // 硬编码已知域名兜底（DNS UDP 在某些网络环境下会失败）
    if (strcmp(hostname, "api.tenclass.net") == 0) {
        // 尝试 DNS，失败则用硬编码 IP
        if (manual_dns_resolve(hostname, ip_out, ip_out_len)) {
            return true;
        }
        ESP_LOGW(TAG, "DNS failed for api.tenclass.net, using fallback IP");
        snprintf(ip_out, ip_out_len, "112.74.84.224");
        return true;
    }
    if (strcmp(hostname, "wss.tenclass.net") == 0) {
        if (manual_dns_resolve(hostname, ip_out, ip_out_len)) {
            return true;
        }
        ESP_LOGW(TAG, "DNS failed for wss.tenclass.net, using fallback IP");
        snprintf(ip_out, ip_out_len, "112.74.84.224");
        return true;
    }
    return manual_dns_resolve(hostname, ip_out, ip_out_len);
}

static void clear_ringbuffers(void) {
    s_tts_stream_active = false;
    if (s_opus_ring_buf) {
        size_t size;
        while (1) {
            void *item = xRingbufferReceive(s_opus_ring_buf, &size, 0);
            if (!item) break;
            vRingbufferReturnItem(s_opus_ring_buf, item);
        }
    }
    if (s_play_ring_buf) {
        size_t size;
        while (1) {
            void *item = xRingbufferReceive(s_play_ring_buf, &size, 0);
            if (!item) break;
            vRingbufferReturnItem(s_play_ring_buf, item);
        }
    }
    if (s_ws_rx_assembly_buf) {
        free(s_ws_rx_assembly_buf);
        s_ws_rx_assembly_buf = NULL;
    }
    s_ws_rx_assembly_len = 0;
    s_ws_rx_assembly_alloc_sz = 0;

    if (s_opus_assembly_buf) {
        free(s_opus_assembly_buf);
        s_opus_assembly_buf = NULL;
    }
    s_opus_assembly_len = 0;
    s_opus_assembly_alloc_sz = 0;
    s_opus_assembly_timestamp = 0;
    ESP_LOGI(TAG, "Audio RingBuffers and assembly cache cleared and reset");
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            update_state(XIAOZHI_STATE_CONNECTED, "WebSocket Connected");
            clear_ringbuffers();
            s_server_hello_received = false;
            esp_wifi_set_ps(WIFI_PS_NONE);
            
            // Keep boot fast and avoid test audio interfering with XiaoZhi session setup.

            ESP_LOGI(TAG, "=== Sending V2 HELLO (Opus Mode) ===");
            // Request V2 protocol - use Opus for both upload and download
            send_json_msg("{\"type\":\"hello\",\"version\":2,\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":20}}");
            ESP_LOGI(TAG, "V2 HELLO sent, waiting for server response...");
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x01) { // TEXT
                if (data->data_len > 0) {
                    char *json = (char*)malloc(data->data_len + 1);
                    if (json) {
                        memcpy(json, data->data_ptr, data->data_len);
                        json[data->data_len] = 0;
                        handle_server_msg(json);
                        free(json);
                    }
                }
            } else if (data->op_code == 0x02 || data->op_code == 0x00) { // BINARY or CONTINUATION
                bool allow_pass = s_tts_stream_active;
                if (data->data_len >= sizeof(xiaozhi_v2_header_t) && data->payload_offset == 0) {
                    const xiaozhi_v2_header_t *header = (const xiaozhi_v2_header_t *)data->data_ptr;
                    uint16_t version = ntohs(header->version);
                    uint16_t type = ntohs(header->type);
                    if (version == 2) {
                        if (type == 0) { // Audio
                            if (!s_tts_stream_active) {
                                s_tts_stream_active = true;
                                s_restore_capture_pending = false;
                                s_decoder_sample_rate = 16000;
                                s_last_packet_time = esp_timer_get_time() / 1000;
                                prepare_tts_output(s_decoder_sample_rate);
                                ESP_LOGI(TAG, "TTS Stream Auto-Activated via V2 binary header");
                                update_state(XIAOZHI_STATE_SPEAKING, "Speaking");
                            }
                            allow_pass = true;
                        } else if (type == 1) { // JSON
                            allow_pass = true;
                        }
                    }
                }
                if (!allow_pass) {
                    break;
                }
                if (s_opus_ring_buf && data->data_len > 0) {
                    if (data->payload_offset == 0) {
                        s_ws_rx_assembly_len = 0;
                        if (data->data_len == data->payload_len) {
                            BaseType_t ret = xRingbufferSend(s_opus_ring_buf, data->data_ptr, data->data_len, pdMS_TO_TICKS(200));
                            if (ret != pdTRUE) {
                                ESP_LOGW(TAG, "WS: ringbuffer send failed (single frame)!");
                            }
                        } else {
                            if (s_ws_rx_assembly_alloc_sz < data->payload_len) {
                                uint8_t *new_assembly = (uint8_t *)heap_caps_realloc(s_ws_rx_assembly_buf, data->payload_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                                if (new_assembly) {
                                    s_ws_rx_assembly_buf = new_assembly;
                                    s_ws_rx_assembly_alloc_sz = data->payload_len;
                                } else {
                                    ESP_LOGE(TAG, "Failed to allocate ws assembly buf of sz %d in PSRAM", (int)data->payload_len);
                                    break;
                                }
                            }
                            memcpy(s_ws_rx_assembly_buf, data->data_ptr, data->data_len);
                            s_ws_rx_assembly_len = data->data_len;
                        }
                    } else {
                        if (s_ws_rx_assembly_buf && s_ws_rx_assembly_len == data->payload_offset) {
                            if (s_ws_rx_assembly_len + data->data_len <= s_ws_rx_assembly_alloc_sz) {
                                memcpy(s_ws_rx_assembly_buf + data->payload_offset, data->data_ptr, data->data_len);
                                s_ws_rx_assembly_len += data->data_len;
                                if (s_ws_rx_assembly_len == data->payload_len) {
                                    BaseType_t ret = xRingbufferSend(s_opus_ring_buf, s_ws_rx_assembly_buf, s_ws_rx_assembly_len, pdMS_TO_TICKS(200));
                                    if (ret != pdTRUE) {
                                        ESP_LOGW(TAG, "WS: ringbuffer send failed (assembled frame)!");
                                    }
                                    s_ws_rx_assembly_len = 0;
                                }
                            } else {
                                ESP_LOGE(TAG, "WS assembly overrun: len=%u, offset=%u, alloc=%u",
                                         (unsigned)data->data_len, (unsigned)data->payload_offset, (unsigned)s_ws_rx_assembly_alloc_sz);
                                s_ws_rx_assembly_len = 0;
                            }
                        } else {
                            ESP_LOGW(TAG, "WS segment offset mismatch: offset=%u, expected=%u",
                                     (unsigned)data->payload_offset, (unsigned)s_ws_rx_assembly_len);
                            s_ws_rx_assembly_len = 0;
                        }
                    }
                }
            } else if (data->op_code == 0x08) {
                ESP_LOGW(TAG, "WS: close frame received, len=%d", data->data_len);
                // 不再触发退出任务，改由主循环在后台自动重连以防内存泄漏并保持永远在线
            } else if (data->op_code == 0x09) {
                ESP_LOGD(TAG, "WS: PING received");
            } else if (data->op_code == 0x0A) {
                ESP_LOGV(TAG, "WS: PONG received");
            } else {
                ESP_LOGW(TAG, "WS: unknown op_code=%d, len=%d", data->op_code, data->data_len);
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected: hello=%d, ptt=%d", s_server_hello_received, s_ptt_active);
            s_ptt_active = false;
            s_listen_session_active = false;
            memset(s_session_id, 0, sizeof(s_session_id));
            s_server_hello_received = false;
            s_waiting_for_abort_ack = false;
            s_abort_sent_time = 0;
            clear_ringbuffers();
            update_state(XIAOZHI_STATE_IDLE, "Disconnected");
            // 不退任务，保留主循环做自动重连
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error event received");
            s_ptt_active = false;
            s_listen_session_active = false;
            s_server_hello_received = false;
            s_waiting_for_abort_ack = false;
            s_abort_sent_time = 0;
            update_state(XIAOZHI_STATE_ERROR, "WebSocket Error");
            // 不退任务，保留主循环做自动重连
            break;
        default: break;
    }
}

static esp_err_t check_activation(const char* device_id, char* token_out, char* uri_out) {
    char api_url[256];
    
    while (!s_should_stop) {
        update_state(XIAOZHI_STATE_ACTIVATING, "Checking Activation...");
        
        // Resolve api.tenclass.net to IP (ESP-Hosted getaddrinfo is broken)
        char api_ip[48] = {0};
        if (!resolve_host_to_ip("api.tenclass.net", api_ip, sizeof(api_ip))) {
            ESP_LOGW(TAG, "Failed to resolve api.tenclass.net, retrying in 5s");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        snprintf(api_url, sizeof(api_url), "https://%s/xiaozhi/ota/", api_ip);
        ESP_LOGI(TAG, "Activation URL: %s", api_url);
        
        esp_http_client_config_t config = {
            .url = api_url,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
            .common_name = "api.tenclass.net",  // SNI must be hostname; CN check passes since cert is for this domain
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        // Set Host header for proper HTTP routing
        esp_http_client_set_header(client, "Host", "api.tenclass.net");
        
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Device-Id", device_id);
        esp_http_client_set_header(client, "Client-Id", "12345678-1234-1234-1234-123456789012");
        esp_http_client_set_header(client, "Activation-Version", "1");
        esp_err_t err = esp_http_client_open(client, 2);
        if (err == ESP_OK) {
            esp_http_client_write(client, "{}", 2);
            esp_http_client_fetch_headers(client);
            int status_code = esp_http_client_get_status_code(client);
            
            if (status_code == 200) {
                char *buf = malloc(2048);
                if (buf) {
                    int total_len = 0;
                    int read_len = 0;
                    while (1) {
                        read_len = esp_http_client_read(client, buf + total_len, 2047 - total_len);
                        if (read_len <= 0) break;
                        total_len += read_len;
                    }
                    
                    if (total_len > 0) {
                        buf[total_len] = 0;
                        cJSON *root = cJSON_Parse(buf);
                        if (root) {
                            cJSON *activation = cJSON_GetObjectItem(root, "activation");
                            if (cJSON_IsObject(activation)) {
                                cJSON *code = cJSON_GetObjectItem(activation, "code");
                                if (cJSON_IsString(code)) {
                                    ESP_LOGW(TAG, "=========================================");
                                    ESP_LOGW(TAG, "Device not activated!");
                                    ESP_LOGW(TAG, "Please visit xiaozhi.me and enter code:");
                                    ESP_LOGW(TAG, ">>> %s <<<", code->valuestring);
                                    ESP_LOGW(TAG, "=========================================");
                                    update_state(XIAOZHI_STATE_ACTIVATING, "Waiting for activation...");
                                    s_activation_pending = true;
                                    cJSON_Delete(root);
                                    free(buf);
                                    esp_http_client_cleanup(client);
                                    return ESP_ERR_NOT_FINISHED;
                                }
                            } else {
                                cJSON *ws = cJSON_GetObjectItem(root, "websocket");
                                if (cJSON_IsObject(ws)) {
                                    cJSON *token = cJSON_GetObjectItem(ws, "token");
                                    cJSON *url = cJSON_GetObjectItem(ws, "url");
                                    if (cJSON_IsString(token) && cJSON_IsString(url)) {
                                        strcpy(token_out, token->valuestring);
                                        strcpy(uri_out, url->valuestring);
                                        ESP_LOGI(TAG, "Activation successful. Token and URI retrieved.");
                                        cJSON_Delete(root);
                                        free(buf);
                                        esp_http_client_cleanup(client);
                                        return ESP_OK;
                                    }
                                }
                            }
                            cJSON_Delete(root);
                        } else {
                            ESP_LOGE(TAG, "Failed to parse JSON response");
                        }
                    } else {
                        ESP_LOGE(TAG, "Failed to read HTTP response body");
                    }
                    free(buf);
                }
            } else {
                ESP_LOGE(TAG, "HTTP POST failed: code %d", status_code);
            }
        } else {
            ESP_LOGE(TAG, "HTTP POST request failed to open: %s", esp_err_to_name(err));
        }
        
        esp_http_client_cleanup(client);
        vTaskDelay(pdMS_TO_TICKS(5000)); // Poll every 5 seconds
    }
    
    return ESP_FAIL;
}

static void audio_capture_task(void* pvParameters) {
    int16_t stereo_buffer[FRAME_SIZE * 2]; 
    int16_t mono_buffer[FRAME_SIZE];
    size_t bytes_read;
    static int tx_count = 0;
    
    // Always use STEREO for BSP compatibility
    restore_capture_output();
    ESP_LOGI(TAG, "Mic Task: initialized at %d Hz STEREO", SAMPLE_RATE);
    
    ESP_LOGI(TAG, "Mic Task: s_opus_encoder=%p", s_opus_encoder);
    
    // Set encoder to VOIP mode with lower complexity for stability
    if (s_opus_encoder) {
        opus_encoder_ctl(s_opus_encoder, OPUS_SET_APPLICATION(OPUS_APPLICATION_VOIP));
        opus_encoder_ctl(s_opus_encoder, OPUS_SET_BITRATE(12000));
        opus_encoder_ctl(s_opus_encoder, OPUS_SET_COMPLEXITY(0));
        ESP_LOGI(TAG, "Mic Task: Opus encoder configured for low-stack mode");
    }
    
    bool last_capture_active = false;
    while (!s_should_stop) {
        bool capture_active = s_ptt_active && s_state != XIAOZHI_STATE_SPEAKING && s_ws_client && esp_websocket_client_is_connected(s_ws_client);
        if (capture_active != last_capture_active) {
            ESP_LOGI(TAG, "Mic Task: capture %s (ptt=%d, state=%d, ws=%d)",
                     capture_active ? "active" : "idle",
                     s_ptt_active,
                     (int)s_state,
                     (s_ws_client && esp_websocket_client_is_connected(s_ws_client)) ? 1 : 0);
            last_capture_active = capture_active;
        }
        if (capture_active) {
            esp_err_t ret = bsp_extra_i2s_read(stereo_buffer, sizeof(stereo_buffer), &bytes_read, 100);
            if (ret == ESP_OK && bytes_read > 0) {
                int samples = bytes_read / 4; 
                ESP_LOGD(TAG, "Mic Task: read %d bytes -> %d samples", bytes_read, samples);
                
                // Downmix Stereo to Mono
                for (int i = 0; i < samples; i++) {
                    mono_buffer[i] = (stereo_buffer[i*2] + stereo_buffer[i*2+1]) / 2;
                }

                // Encode PCM directly into s_mic_v2_buf payload to achieve Zero-Copy
                if (s_opus_encoder && s_mic_v2_buf) {
                    UBaseType_t stack_words = uxTaskGetStackHighWaterMark(NULL);
                    if (tx_count < 5 || tx_count % 20 == 0) {
                        ESP_LOGI(TAG, "Mic Task: stack high watermark=%u words before encode", (unsigned)stack_words);
                    }
                    
                    uint8_t *encode_dest = s_mic_v2_buf + sizeof(xiaozhi_v2_header_t);
                    int encoded_bytes = opus_encode(s_opus_encoder, mono_buffer, samples, encode_dest, 256);
                    if (encoded_bytes > 0) {
                        xiaozhi_v2_header_t *header = (xiaozhi_v2_header_t *)s_mic_v2_buf;
                        header->version = htons(2);
                        header->type = htons(0);
                        header->reserved = 0;
                        header->timestamp = htonl((uint32_t)(esp_timer_get_time() / 1000ULL));
                        header->payload_size = htonl((uint32_t)encoded_bytes);

                        size_t packet_size = sizeof(xiaozhi_v2_header_t) + encoded_bytes;
                        int sent = esp_websocket_client_send_bin(s_ws_client, (const char*)s_mic_v2_buf, packet_size, pdMS_TO_TICKS(100));
                        if (sent < 0) {
                            ESP_LOGW(TAG, "Mic Task: send_bin failed for %u bytes", (unsigned)packet_size);
                            continue;
                        }

                        tx_count++;
                        if (tx_count <= 5 || tx_count % 20 == 0) {
                            ESP_LOGI(TAG, "Mic Task: TX V2 Opus payload=%d packet=%u count=%d", encoded_bytes, (unsigned)packet_size, tx_count);
                        }
                    } else {
                        ESP_LOGW(TAG, "Mic Task: opus_encode returned %d (samples=%d)", encoded_bytes, samples);
                    }
                } else {
                    ESP_LOGE(TAG, "Mic Task: encoder not ready! s_opus_encoder=%p", s_opus_encoder);
                }
            } else if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Mic Task: i2s_read failed: %s", esp_err_to_name(ret));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    
    s_audio_task_handle = NULL;
    vTaskDelete(NULL);
}

static void audio_decode_task(void *pvParameters) {
    size_t item_size;
    static int decode_counter = 0;
    s_last_packet_time = esp_timer_get_time() / 1000;

    while (!s_should_stop) {
        char *item = (char *)xRingbufferReceive(s_opus_ring_buf, &item_size, pdMS_TO_TICKS(100));
        if (item) {
            s_last_packet_time = esp_timer_get_time() / 1000;
            const uint8_t *payload = (const uint8_t *)item;
            size_t payload_size = item_size;
            bool handled = false;

            if (item_size >= sizeof(xiaozhi_v2_header_t)) {
                xiaozhi_v2_header_t *header = (xiaozhi_v2_header_t *)item;
                uint16_t version = ntohs(header->version);
                uint16_t type = ntohs(header->type);
                uint32_t framed_payload_size = ntohl(header->payload_size);

                if (version == 2 && framed_payload_size > 0 && item_size >= sizeof(xiaozhi_v2_header_t) + framed_payload_size) {
                    payload = (const uint8_t *)item + sizeof(xiaozhi_v2_header_t);
                    payload_size = framed_payload_size;
                    handled = true;

                    if (type == 1) { // JSON
                        char *json_txt = (char *)malloc(payload_size + 1);
                        if (json_txt) {
                            memcpy(json_txt, payload, payload_size);
                            json_txt[payload_size] = 0;
                            handle_server_msg(json_txt);
                            free(json_txt);
                        }
                        vRingbufferReturnItem(s_opus_ring_buf, (void *)item);
                        continue;
                    }

                    if (type == 0) { // Audio
                        // Skip Ogg Opus container identification and metadata headers
                        if (payload_size >= 8 && memcmp(payload, "OpusHead", 8) == 0) {
                            ESP_LOGI(TAG, "Skipping Ogg OpusHead container header");
                            vRingbufferReturnItem(s_opus_ring_buf, (void *)item);
                            continue;
                        }
                        if (payload_size >= 8 && memcmp(payload, "OpusTags", 8) == 0) {
                            ESP_LOGI(TAG, "Skipping Ogg OpusTags container header");
                            vRingbufferReturnItem(s_opus_ring_buf, (void *)item);
                            continue;
                        }

                        // Reassemble split Ogg packets sent in segments (max segment size is 255 bytes)
                        uint32_t current_ts = ntohl(header->timestamp);
                        if (s_opus_assembly_len > 0 && current_ts != s_opus_assembly_timestamp) {
                            // The previous accumulated packet is complete (timestamp mismatch). Decode it now.
                            if (s_opus_decoder && s_opus_decode_buf) {
                                int decoded_samples = opus_decode(s_opus_decoder, s_opus_assembly_buf, (opus_int32)s_opus_assembly_len, s_opus_decode_buf, 2880, 0);
                                if (decoded_samples > 0) {
                                    size_t decoded_bytes = decoded_samples * sizeof(int16_t);
                                    if (xRingbufferSend(s_play_ring_buf, s_opus_decode_buf, decoded_bytes, pdMS_TO_TICKS(100)) != pdTRUE) {
                                        ESP_LOGW(TAG, "Speaker ringbuffer full, dropped %u bytes PCM", (unsigned)decoded_bytes);
                                    }
                                }
                            }
                            s_opus_assembly_len = 0;
                        }

                        if (payload_size == 255) {
                            if (s_opus_assembly_len == 0) {
                                s_opus_assembly_timestamp = current_ts;
                            }
                            if (s_opus_assembly_len + payload_size > s_opus_assembly_alloc_sz) {
                                size_t new_sz = s_opus_assembly_len + payload_size + 512;
                                uint8_t *new_buf = (uint8_t *)heap_caps_realloc(s_opus_assembly_buf, new_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                                if (!new_buf) {
                                    ESP_LOGE(TAG, "Failed to reallocate s_opus_assembly_buf to %d in PSRAM", (int)new_sz);
                                    vRingbufferReturnItem(s_opus_ring_buf, (void *)item);
                                    continue;
                                }
                                s_opus_assembly_buf = new_buf;
                                s_opus_assembly_alloc_sz = new_sz;
                            }
                            memcpy(s_opus_assembly_buf + s_opus_assembly_len, payload, payload_size);
                            s_opus_assembly_len += payload_size;
                            vRingbufferReturnItem(s_opus_ring_buf, (void *)item);
                            continue; // Wait for the remaining segment(s)
                        } else {
                            if (s_opus_assembly_len > 0) {
                                if (s_opus_assembly_len + payload_size > s_opus_assembly_alloc_sz) {
                                    size_t new_sz = s_opus_assembly_len + payload_size;
                                    uint8_t *new_buf = (uint8_t *)heap_caps_realloc(s_opus_assembly_buf, new_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                                    if (!new_buf) {
                                        ESP_LOGE(TAG, "Failed to reallocate s_opus_assembly_buf to %d in PSRAM", (int)new_sz);
                                        s_opus_assembly_len = 0;
                                        vRingbufferReturnItem(s_opus_ring_buf, (void *)item);
                                        continue;
                                    }
                                    s_opus_assembly_buf = new_buf;
                                    s_opus_assembly_alloc_sz = new_sz;
                                }
                                memcpy(s_opus_assembly_buf + s_opus_assembly_len, payload, payload_size);
                                s_opus_assembly_len += payload_size;
                                payload = s_opus_assembly_buf;
                                payload_size = s_opus_assembly_len;
                            }
                        }
                    }

                    if (decode_counter < 5) {
                        ESP_LOGI(TAG, "Audio Pipeline: V2 frame ver=%u type=%u payload=%u total=%u",
                                 (unsigned)version,
                                 (unsigned)type,
                                 (unsigned)framed_payload_size,
                                 (unsigned)item_size);
                    }
                }
            }

            if (s_opus_decoder && s_opus_decode_buf) {
                int decoded_samples;
                bool was_plc = false;

                if (payload_size <= 10) {
                    // Trigger PLC (Packet Loss Concealment) comfort noise for extremely small or empty DTX packets
                    decoded_samples = opus_decode(s_opus_decoder, NULL, 0, s_opus_decode_buf, 320, 0);
                    was_plc = true;
                } else {
                    decoded_samples = opus_decode(s_opus_decoder, payload, (opus_int32)payload_size, s_opus_decode_buf, 2880, 0);
                    if (decoded_samples < 0 && payload_size < 80) {
                        // Dynamic fallback: if a relatively small audio/DTX packet fails standard decode, treat it as a lost frame and run PLC
                        decoded_samples = opus_decode(s_opus_decoder, NULL, 0, s_opus_decode_buf, 320, 0);
                        was_plc = true;
                    }
                }

                if (decoded_samples > 0) {
                    size_t decoded_bytes = decoded_samples * sizeof(int16_t);
                    /* Best-effort send: if play buffer is full, drop PCM.
                       Always consume Opus to keep the WS→Opus pipeline free. */
                    if (xRingbufferSend(s_play_ring_buf, s_opus_decode_buf, decoded_bytes, pdMS_TO_TICKS(100)) != pdTRUE) {
                        ESP_LOGW(TAG, "Speaker ringbuffer full, dropped %u bytes PCM", (unsigned)decoded_bytes);
                    }
                    if (++decode_counter <= 5 || decode_counter % 50 == 0) {
                        if (was_plc) {
                            ESP_LOGI(TAG, "Audio Pipeline: comfort noise/PLC active (%d samples, payload=%u)",
                                     decoded_samples, (unsigned)payload_size);
                        } else {
                            ESP_LOGI(TAG, "Audio Pipeline: decode active (%d samples @ %d Hz, framed=%d, payload=%u)",
                                     decoded_samples,
                                     s_decoder_sample_rate,
                                     handled ? 1 : 0,
                                     (unsigned)payload_size);
                        }
                    }
                } else {
                    // Only log error for non-PLC failure packets
                    if (!was_plc) {
                        ESP_LOGW(TAG, "Opus decode failed: %d (framed=%d, payload=%u, rate=%d)",
                                 decoded_samples,
                                 handled ? 1 : 0,
                                 (unsigned)payload_size,
                                 s_decoder_sample_rate);
                        if (payload && payload_size > 0) {
                            char hex_str[64] = {0};
                            int log_len = payload_size < 16 ? payload_size : 16;
                            for (int i = 0; i < log_len; i++) {
                                sprintf(hex_str + i * 3, "%02X ", payload[i]);
                            }
                            char ascii_str[17] = {0};
                            for (int i = 0; i < log_len; i++) {
                                char c = payload[i];
                                ascii_str[i] = (c >= 32 && c <= 126) ? c : '.';
                            }
                            ESP_LOGW(TAG, "  Payload hex: %s | ascii: %s", hex_str, ascii_str);
                        }
                    }
                }
            }

            s_opus_assembly_len = 0; // Clear assembly length for next packet
            vRingbufferReturnItem(s_opus_ring_buf, (void *)item);
        } else {
            if (s_tts_stream_active) {
                uint32_t now = esp_timer_get_time() / 1000;
                if (now - s_last_packet_time > 1500) {
                    ESP_LOGI(TAG, "TTS Timeout (1.5s idle) - auto stopping stream");
                    s_tts_stream_active = false;
                    s_restore_capture_pending = true;
                    s_tts_stop_time = esp_timer_get_time();
                    update_state(XIAOZHI_STATE_CONNECTED, "TTS Done");
                }
            }
            if (s_state != XIAOZHI_STATE_SPEAKING) {
                decode_counter = 0;
            }
        }
    }
    s_decode_task_handle = NULL;
    vTaskDelete(NULL);
}

static void speaker_task(void *pvParameters) {
    size_t item_size;
    // Larger stereo chunk for fewer I2S_DMA round trips (was 512, now 1024)
    static int16_t stereo_chunk[1024];
    ESP_LOGI(TAG, "Speaker task started");

    bsp_extra_player_init();
    int current_vol = ui_get_current_volume();
    bsp_extra_codec_volume_set(current_vol, NULL);
    bsp_extra_codec_mute_set(false);

    ESP_LOGI(TAG, "Speaker: player initialized, volume=%d, unmuted", current_vol);

    bool buffering = true;

    while (!s_should_stop) {
        if (buffering) {
            UBaseType_t free_pos = 0, read_pos = 0, write_pos = 0, acquire_pos = 0, items_waiting = 0;
            vRingbufferGetInfo(s_play_ring_buf, &free_pos, &read_pos, &write_pos, &acquire_pos, &items_waiting);
            size_t occupied_items = (size_t)items_waiting;
            // Pre-buffer 6 frames for NOSPLIT ringbuffer (approx 120ms of audio)
            if (occupied_items < 6) {
                vTaskDelay(pdMS_TO_TICKS(occupied_items == 0 ? 50 : 10));
                continue;
            }
            buffering = false;
            ESP_LOGI(TAG, "Pre-buffering complete, starting playback (occupied=%u items)", (unsigned)occupied_items);
        }
        int16_t *item = (int16_t *)xRingbufferReceive(s_play_ring_buf, &item_size, pdMS_TO_TICKS(100));
        if (item) {
            size_t mono_samples = item_size / sizeof(int16_t);
            size_t offset = 0;
            s_restore_capture_pending = false;

            while (offset < mono_samples && !s_should_stop) {
                size_t remaining = mono_samples - offset;
                size_t chunk_samples = (sizeof(stereo_chunk) / sizeof(stereo_chunk[0])) / 2;
                if (remaining < chunk_samples) chunk_samples = remaining;

                int16_t *src = item + offset;
                int16_t *dst = stereo_chunk;
                for (size_t i = 0; i < chunk_samples; i++) {
                    int16_t s = src[i];
                    *dst++ = s;
                    *dst++ = s;
                }

                size_t bytes_written = 0;
                size_t stereo_bytes = chunk_samples * 2 * sizeof(int16_t);
                esp_err_t ret = ESP_OK;
                
                if (s_audio_mutex && xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    ret = bsp_extra_i2s_write(stereo_chunk, stereo_bytes, &bytes_written, portMAX_DELAY);
                    xSemaphoreGive(s_audio_mutex);
                } else {
                    ESP_LOGE(TAG, "Speaker: failed to take audio mutex for write!");
                    ret = ESP_ERR_TIMEOUT;
                }
                
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Speaker: write failed: %s", esp_err_to_name(ret));
                    break;
                }
                if (bytes_written < stereo_bytes) {
                    ESP_LOGW(TAG, "Speaker: partial write %u/%u", (unsigned)bytes_written, (unsigned)stereo_bytes);
                }
                offset += chunk_samples;
            }
            vRingbufferReturnItem(s_play_ring_buf, (void *)item);
        } else {
            // Buffer ran dry! Write a chunk of silence to I2S to drain the DMA buffer to 0
            // and prevent sharp DC offsets from causing pop sounds (音爆)
            memset(stereo_chunk, 0, sizeof(stereo_chunk));
            size_t bytes_written = 0;
            if (s_audio_mutex && xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                bsp_extra_i2s_write(stereo_chunk, sizeof(stereo_chunk), &bytes_written, pdMS_TO_TICKS(50));
                xSemaphoreGive(s_audio_mutex);
            }

            buffering = true;
            if (s_restore_capture_pending) {
                if (restore_capture_output() == ESP_OK) {
                    ESP_LOGI(TAG, "Capture sample rate restored after speaker drain");
                } else {
                    ESP_LOGW(TAG, "Failed to restore capture sample rate after speaker drain");
                }
                s_restore_capture_pending = false;
            }
        }
    }
    s_speaker_task_handle = NULL;
    vTaskDelete(NULL);
}

void xiaozhi_ai_service_test_speaker(void) {
    ESP_LOGI(TAG, "Hardware Test: skipped during XiaoZhi session bring-up");
}

static void xiaozhi_main_task(void* pvParameters) {
    if (s_http_tts_mode) {
        while (!s_should_stop) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char device_id[18];
    snprintf(device_id, sizeof(device_id), "%02x:%02x:%02x:%02x:%02x:%02x", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
             
    esp_netif_dns_info_t dns;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
        if (dns.ip.u_addr.ip4.addr == 0 || dns.ip.u_addr.ip4.addr == 0xFFFFFFFF) {
            dns.ip.u_addr.ip4.addr = ipaddr_addr("114.114.114.114");
            dns.ip.type = IPADDR_TYPE_V4;
            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
        }
    }
             
    char token[128] = {0};
    char ws_uri[256] = {0};

    // 直接使用自建多模态服务器的 WebSocket 连接地址，根据通信协议 v3.0 附带连接参数，采用路径 /p4/ws
    snprintf(ws_uri, sizeof(ws_uri), "ws://*.*.*.*/p4/ws?device_id=%s&device_type=esp32_p4&firmware_version=1.0.0", device_id); // Censored Server API / 已脱敏服务器API
    ESP_LOGI(TAG, "Connecting to self-built multimodal model service: %s", ws_uri);

    char client_id[64];
    snprintf(client_id, sizeof(client_id), "esp32-%02x%02x%02x%02x%02x%02x-001", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = ws_uri;
    ws_cfg.transport = WEBSOCKET_TRANSPORT_OVER_TCP; // 改为非加密 TCP 传输
    ws_cfg.reconnect_timeout_ms = 1000;
    ws_cfg.network_timeout_ms = 15000;

    char headers[512];
    snprintf(headers, sizeof(headers), 
             "Protocol-Version: 2\r\n"
             "Device-Id: %s\r\n"
             "Client-Id: %s\r\n",
             device_id, client_id);
    ws_cfg.headers = headers;
    ws_cfg.buffer_size = WS_BUFFER_SIZE;
    ws_cfg.task_prio = 8;
    ws_cfg.task_stack = 8 * 1024; // 8KB stack (reduced to save internal RAM for SDIO)
    ws_cfg.reconnect_timeout_ms = 1000;
    ws_cfg.network_timeout_ms = 30000; // [CRITICAL] 30s timeout to survive bursts
    ws_cfg.ping_interval_sec = 5;      // [STABILITY] Heartbeat every 5s
    ws_cfg.pingpong_timeout_sec = 60;   // [STABILITY] Long tolerance for PONG

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    esp_websocket_client_start(s_ws_client);

    int disconnect_check_counter = 0;
    uint32_t ping_counter = 0;
    while (!s_should_stop) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_ws_client) {
            bool ws_connected = esp_websocket_client_is_connected(s_ws_client);
            if (ws_connected) {
                if (s_server_hello_received) {
                    ping_counter++;
                    if (ping_counter >= 30) {
                        ping_counter = 0;
                        send_json_msg("{\"type\":\"ping\"}");
                        ESP_LOGI(TAG, "Sent JSON Ping");
                    }
                } else {
                    ping_counter = 0;
                }

                if (s_waiting_for_abort_ack) {
                    uint64_t now = esp_timer_get_time();
                    if (now - s_abort_sent_time > 3000000ULL) {
                        ESP_LOGE(TAG, "Abort ACK timeout (3s), actively reconnecting client...");
                        s_waiting_for_abort_ack = false;
                        s_server_hello_received = false;
                        memset(s_session_id, 0, sizeof(s_session_id));
                        update_state(XIAOZHI_STATE_IDLE, "Reconnecting");
                        
                        esp_websocket_client_stop(s_ws_client);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_websocket_client_start(s_ws_client);
                        disconnect_check_counter = 0;
                        continue;
                    }
                }
                if (s_server_hello_received) {
                    disconnect_check_counter = 0;
                } else {
                    disconnect_check_counter++;
                    // 连续 5 秒网络物理连通但应用层未完成握手（半开连接假死），强制重置并重建连接
                    if (disconnect_check_counter >= 5) {
                        ESP_LOGI(TAG, "Half-open connection detected (connected but no HELLO response for 5s), actively restarting client...");
                        s_server_hello_received = false;
                        memset(s_session_id, 0, sizeof(s_session_id));
                        update_state(XIAOZHI_STATE_IDLE, "Reconnecting");
                        
                        esp_websocket_client_stop(s_ws_client);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_websocket_client_start(s_ws_client);
                        disconnect_check_counter = 0;
                    }
                }
            } else {
                // 底层已断连，完全交由驱动层 1000ms 极速自动重连，避开并发 stop 导致底层重连线程发生资源竞争死锁
                disconnect_check_counter = 0;
            }
        }
    }

    if (s_ws_client) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }
    s_running = false;
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t xiaozhi_ai_service_init(void) {
    // PCM buffer: 32KB, allocated in PSRAM to save internal SRAM for DMA2D
    s_play_ring_buf = xRingbufferCreateWithCaps(PCM_RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // Opus buffer: 64KB, PSRAMallocated in PSRAM
    s_opus_ring_buf = xRingbufferCreateWithCaps(OPUS_RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (s_play_ring_buf == NULL || s_opus_ring_buf == NULL) {
        ESP_LOGE(TAG, "Failed allocate RingBuffers in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    if (s_audio_mutex == NULL) {
        s_audio_mutex = xSemaphoreCreateMutex();
    }

    // Keep playback/capture at the uplink rate until server TTS announces its rate.
    bsp_extra_player_init();
    bsp_extra_codec_set_fs(SAMPLE_RATE, 16, I2S_SLOT_MODE_STEREO);
    int init_vol = ui_get_current_volume();
    bsp_extra_codec_volume_set(init_vol, NULL);
    bsp_extra_codec_mute_set(false);
    s_codec_sample_rate = SAMPLE_RATE;
    
    // Allocate 2880 samples * 2 bytes = 5760 bytes for stable decoding in PSRAM
    s_opus_decode_buf = (int16_t *)heap_caps_malloc(2880 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_opus_decode_buf) return ESP_ERR_NO_MEM;
    
    // Create Opus Decoder using the protocol default and switch dynamically on TTS start.
    int err;
    if (recreate_decoder_if_needed(SAMPLE_RATE) != ESP_OK) {
        return ESP_FAIL;
    }
    
    // Create Opus Encoder for microphone upload (16kHz)
    s_opus_encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || s_opus_encoder == NULL) {
        ESP_LOGE(TAG, "FAILED: Opus encoder create err=%d, encoder=%p", err, s_opus_encoder);
    } else {
        ESP_LOGI(TAG, "SUCCESS: Opus encoder created (16kHz, err=%d), encoder=%p", err, s_opus_encoder);
        // Initialize encoder with explicit settings
        opus_encoder_ctl(s_opus_encoder, OPUS_SET_BITRATE(12000));
        opus_encoder_ctl(s_opus_encoder, OPUS_SET_COMPLEXITY(0));
        opus_encoder_ctl(s_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        ESP_LOGI(TAG, "Opus encoder configured for low-stack mode");
    }
    
    // Pre-allocate Mic V2 Send Buffer (Header + Max PCM)
    s_mic_v2_buf = (uint8_t*)heap_caps_malloc(sizeof(xiaozhi_v2_header_t) + 4000, MALLOC_CAP_SPIRAM);
    
    // Core 1: Tasks will be dynamically created/destroyed inside start/stop to ensure robust restart
    return ESP_OK;
}

esp_err_t xiaozhi_ai_service_start(void) {
    if (s_running) {
        ESP_LOGI(TAG, "Service already running, ignoring restart request");
        return ESP_OK; // 直接返回 OK，防止 UI 误报启动冲突错误
    }
    s_running = true;
    s_should_stop = false;
    s_activation_pending = false;

    // Enable PA (Power Amplifier) for speaker on GPIO 53
    ESP_LOGI(TAG, "Enabling Speaker PA on GPIO 53...");
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << 53);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)53, 1);

    // 动态申请栈内存 (主任务栈必须在内部 SRAM 中，以支持低层驱动操作；缩减为 8KB 以保障 SDIO 网卡内存)
    s_main_task_stack = (StackType_t *)heap_caps_malloc(8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_audio_task_stack = (StackType_t *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    s_decode_task_stack = (StackType_t *)heap_caps_malloc(24576, MALLOC_CAP_SPIRAM);
    s_speaker_task_stack = (StackType_t *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);

    if (!s_main_task_stack || !s_audio_task_stack || !s_decode_task_stack || !s_speaker_task_stack) {
        ESP_LOGE(TAG, "Failed to allocate static task stacks in PSRAM");
        if (s_main_task_stack) { heap_caps_free(s_main_task_stack); s_main_task_stack = NULL; }
        if (s_audio_task_stack) { heap_caps_free(s_audio_task_stack); s_audio_task_stack = NULL; }
        if (s_decode_task_stack) { heap_caps_free(s_decode_task_stack); s_decode_task_stack = NULL; }
        if (s_speaker_task_stack) { heap_caps_free(s_speaker_task_stack); s_speaker_task_stack = NULL; }
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    // 创建静态任务，栈直接挂载在外部 PSRAM 中，TCB 保留在内部 SRAM 以过调度器校验
    s_task_handle = xTaskCreateStaticPinnedToCore(
        xiaozhi_main_task, "xiaozhi_main",
        8192 / sizeof(StackType_t), NULL, 5,
        s_main_task_stack, &s_main_task_tcb, 0
    );

    s_audio_task_handle = xTaskCreateStaticPinnedToCore(
        audio_capture_task, "mic_task",
        16384 / sizeof(StackType_t), NULL, 5,
        s_audio_task_stack, &s_audio_task_tcb, 0
    );

    s_decode_task_handle = xTaskCreateStaticPinnedToCore(
        audio_decode_task, "decode_task",
        24576 / sizeof(StackType_t), NULL, 6,
        s_decode_task_stack, &s_decode_task_tcb, 0
    );

    s_speaker_task_handle = xTaskCreateStaticPinnedToCore(
        speaker_task, "speaker_task",
        16384 / sizeof(StackType_t), NULL, 7,
        s_speaker_task_stack, &s_speaker_task_tcb, 0
    );

    return ESP_OK;
}

esp_err_t xiaozhi_ai_service_stop(void) {
    if (!s_running) return ESP_OK;
    s_should_stop = true;
    s_server_hello_received = false;
    s_waiting_for_abort_ack = false;
    s_abort_sent_time = 0;
    
    // Wait for tasks to exit their loops gracefully to prevent I2S mutex starvation
    // from forceful vTaskDelete killing tasks while holding mutexes
    int wait_cycles = 20; // max 200ms
    while ((s_task_handle != NULL || s_speaker_task_handle != NULL || s_decode_task_handle != NULL || s_audio_task_handle != NULL) && wait_cycles > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_cycles--;
    }
    
    // Force delete tasks if they are completely stuck
    if (s_task_handle) { vTaskDelete(s_task_handle); s_task_handle = NULL; }
    if (s_audio_task_handle) { vTaskDelete(s_audio_task_handle); s_audio_task_handle = NULL; }
    if (s_decode_task_handle) { vTaskDelete(s_decode_task_handle); s_decode_task_handle = NULL; }
    if (s_speaker_task_handle) { vTaskDelete(s_speaker_task_handle); s_speaker_task_handle = NULL; }
    
    if (s_ws_client) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }

    // 释放动态从外部 PSRAM 中申请的任务栈空间，保持零内存泄漏
    if (s_main_task_stack) { heap_caps_free(s_main_task_stack); s_main_task_stack = NULL; }
    if (s_audio_task_stack) { heap_caps_free(s_audio_task_stack); s_audio_task_stack = NULL; }
    if (s_decode_task_stack) { heap_caps_free(s_decode_task_stack); s_decode_task_stack = NULL; }
    if (s_speaker_task_stack) { heap_caps_free(s_speaker_task_stack); s_speaker_task_stack = NULL; }

    // Clear and purge any legacy or latency frames inside RingBuffers
    clear_ringbuffers();

    s_running = false;
    update_state(XIAOZHI_STATE_IDLE, "Service Stopped");
    return ESP_OK;
}

void xiaozhi_ai_service_set_ptt(bool press) {
    bool ws_connected = (s_ws_client != NULL) && esp_websocket_client_is_connected(s_ws_client);
    ESP_LOGI(TAG, "PTT request: press=%d, hello=%d, ws=%d, session=%s, active=%d", press, s_server_hello_received, ws_connected, s_session_id, s_listen_session_active);
    if (!s_server_hello_received || s_ws_client == NULL || !ws_connected) {
        ESP_LOGW(TAG, "PTT ignored: hello=%d, ws_client=%p, ws_connected=%d", s_server_hello_received, s_ws_client, ws_connected);
        return;
    }

    if (press) {
        // Enforce 1.5s cooldown after TTS stops to allow server ASR task to restart
        uint64_t now = esp_timer_get_time();
        if (s_tts_stop_time > 0 && (now - s_tts_stop_time < 1500000ULL)) {
            ESP_LOGW(TAG, "PTT start ignored: inside post-TTS ASR cooldown (1.5s)");
            return;
        }

        if (s_listen_session_active) {
            ESP_LOGW(TAG, "PTT start ignored: listen session already active");
            s_ptt_active = true;
            return;
        }

        // Active Speaking or Thinking Session Interruption
        if (s_state == XIAOZHI_STATE_SPEAKING || s_state == XIAOZHI_STATE_THINKING) {
            ESP_LOGI(TAG, "PTT: Interrupted active state=%d session. Sending ABORT!", (int)s_state);
            
            // 1. Clear playback buffer immediately so speaker stops playing
            clear_ringbuffers();
            
            // 2. Set abort wait state
            s_waiting_for_abort_ack = true;
            s_abort_sent_time = esp_timer_get_time();
            
            // 3. Send abort frame to notify server to stop generation and TTS
            char abort_json[128];
            snprintf(abort_json, sizeof(abort_json), "{\"session_id\":\"%s\",\"type\":\"abort\"}", s_session_id);
            send_json_msg(abort_json);

            s_ptt_active = true;
            update_state(XIAOZHI_STATE_CONNECTED, "Aborting...");
            return;
        }

        // Purge legacy sound frames before PTT mic capture session starts
        clear_ringbuffers();
        s_ptt_active = true;
        s_listen_session_active = true;
        ESP_LOGI(TAG, "PTT: Start Listening (manual)...");
        char json[128];
        snprintf(json, sizeof(json), "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}", s_session_id);
        send_json_msg(json);
        update_state(XIAOZHI_STATE_LISTENING, "Listening...");
    } else {
        s_ptt_active = false;
        
        if (s_waiting_for_abort_ack) {
            ESP_LOGI(TAG, "PTT released while waiting for abort ACK. Cancel active recording intent.");
            return;
        }

        if (!s_listen_session_active) {
            ESP_LOGW(TAG, "PTT stop ignored: no active listen session");
            return;
        }
        s_listen_session_active = false;
        // Purge stack before waiting for AI agent speaker output starts
        clear_ringbuffers();
        ESP_LOGI(TAG, "PTT: Stop Listening, Trigger thinking...");
        char json[128];
        snprintf(json, sizeof(json), "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}", s_session_id);
        send_json_msg(json);

        // 发送自建服务器对接指南中所要求的停止录音指令
        char stop_listen_json[128];
        snprintf(stop_listen_json, sizeof(stop_listen_json), "{\"type\":\"stop_listen\",\"session_id\":\"%s\"}", s_session_id);
        send_json_msg(stop_listen_json);
        update_state(XIAOZHI_STATE_THINKING, "Thinking...");
    }
}

esp_err_t xiaozhi_ai_service_set_state_callback(xiaozhi_state_cb_t callback) {
    s_state_cb = callback;
    return ESP_OK;
}

esp_err_t xiaozhi_ai_service_set_text_callback(xiaozhi_text_cb_t callback) {
    s_text_cb = callback;
    return ESP_OK;
}

xiaozhi_state_t xiaozhi_ai_service_get_state(void) { return s_state; }
bool xiaozhi_ai_service_is_connected(void) { return s_state >= XIAOZHI_STATE_CONNECTED && s_state != XIAOZHI_STATE_ERROR; }

void xiaozhi_ai_service_send_text(const char* text) {
    if (!text || strlen(text) == 0) return;
    if (s_ws_client && esp_websocket_client_is_connected(s_ws_client)) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "session_id", s_session_id);
        cJSON_AddStringToObject(root, "type", "tts");
        cJSON_AddStringToObject(root, "text", text);
        char *json = cJSON_PrintUnformatted(root);
        if (json) {
            send_json_msg(json);
            free(json);
        }
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Sent TTS text to server: %s", text);
    } else {
        ESP_LOGW(TAG, "WebSocket not connected. Cannot send TTS text.");
    }
}

static char *url_encode(const char *str) {
    static const char hex[] = "0123456789ABCDEF";
    size_t len = strlen(str);
    char *res = malloc(len * 3 + 1);
    if (!res) return NULL;
    char *p = res;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = str[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = c;
        } else {
            *p++ = '%';
            *p++ = hex[c >> 4];
            *p++ = hex[c & 15];
        }
    }
    *p = '\0';
    return res;
}

static void tts_http_task(void *pvParameters) {
    char *text = (char *)pvParameters;
    char *encoded_text = url_encode(text);
    free(text);
    
    if (!encoded_text) {
        s_http_tts_mode = false;
        xiaozhi_ai_service_stop();
        vTaskDelete(NULL);
        return;
    }
    
    char url[512];
    snprintf(url, sizeof(url), "http://*.*.*.*/p4/tts?text=%s", encoded_text); // Censored Server API / 已脱敏服务器API
    free(encoded_text);
    
    ESP_LOGI(TAG, "Starting HTTP TTS GET request to: %s", url);
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        s_http_tts_mode = false;
        xiaozhi_ai_service_stop();
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        s_http_tts_mode = false;
        xiaozhi_ai_service_stop();
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP server returned status code %d", status_code);
        esp_http_client_cleanup(client);
        s_http_tts_mode = false;
        xiaozhi_ai_service_stop();
        vTaskDelete(NULL);
        return;
    }

    // Initialize playback streaming
    s_tts_stream_active = true;
    s_decoder_sample_rate = 16000;
    prepare_tts_output(s_decoder_sample_rate);
    update_state(XIAOZHI_STATE_SPEAKING, "Speaking");
    s_last_packet_time = esp_timer_get_time() / 1000;

    uint8_t *read_buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!read_buf) {
        esp_http_client_cleanup(client);
        s_http_tts_mode = false;
        xiaozhi_ai_service_stop();
        vTaskDelete(NULL);
        return;
    }

    int read_offset = 0;
    bool parsing_header = true;
    uint32_t payload_size = 0;

    while (s_running && !s_should_stop) {
        if (parsing_header) {
            if (read_offset >= 16) {
                xiaozhi_v2_header_t *header = (xiaozhi_v2_header_t *)read_buf;
                uint16_t version = ntohs(header->version);
                uint16_t type = ntohs(header->type);
                payload_size = ntohl(header->payload_size);

                if (version != 2 || payload_size > 2000) {
                    ESP_LOGE(TAG, "Invalid V2 header: version=%d, payload_size=%lu", version, payload_size);
                    break;
                }
                parsing_header = false;
                continue;
            }
        } else {
            if (read_offset >= 16 + payload_size) {
                int frame_len = 16 + payload_size;
                if (s_opus_ring_buf) {
                    BaseType_t ret = xRingbufferSend(s_opus_ring_buf, read_buf, frame_len, pdMS_TO_TICKS(100));
                    if (ret != pdTRUE) {
                        ESP_LOGW(TAG, "Opus ring buffer full, dropped packet");
                    }
                }
                memmove(read_buf, read_buf + frame_len, read_offset - frame_len);
                read_offset -= frame_len;
                parsing_header = true;
                continue;
            }
        }

        int space = 4096 - read_offset;
        if (space <= 0) {
            ESP_LOGE(TAG, "Buffer overflow, resetting offset");
            read_offset = 0;
            parsing_header = true;
            space = 4096;
        }

        int n = esp_http_client_read(client, (char *)(read_buf + read_offset), space);
        if (n > 0) {
            read_offset += n;
        } else if (n == 0) {
            ESP_LOGI(TAG, "HTTP stream reading complete");
            break;
        } else {
            ESP_LOGE(TAG, "HTTP stream read error: %d", n);
            break;
        }
    }

    free(read_buf);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "All TTS packets read and pushed. Waiting for playback to finish...");
    while (s_running && s_tts_stream_active && !s_should_stop) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Playback finished. Stopping voice service.");
    s_http_tts_mode = false;
    xiaozhi_ai_service_stop();
    vTaskDelete(NULL);
}

void xiaoZhi_tts_broadcast(const char *text) {
    if (!text || strlen(text) == 0) return;
    
    // Stop first to ensure clean state
    if (s_running) {
        xiaozhi_ai_service_stop();
    }
    
    s_http_tts_mode = true;
    if (xiaozhi_ai_service_start() == ESP_OK) {
        xTaskCreate(tts_http_task, "tts_http_task", 8192, strdup(text), 5, NULL);
    }
}
