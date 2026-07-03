#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nvs_flash.h"

#include "electronic_nose_gateway.h"

static const char *TAG = "ELEC_NOSE_GW";

#define SYSTEM_PROMPT "You are an electronic nose analysis expert for cold storage and food preservation. "\
"You receive sensor data (Odor, HCHO, CO, VOC, CO2, Temperature, Humidity, UV status). "\
"Analyze the data and return ONLY a valid JSON object (no markdown, no code blocks) with this exact structure:\n"\
"{\n"\
"  \"status\": \"fresh|aging|spoiled\",\n"\
"  \"confidence\": 0.0-1.0,\n"\
"  \"analysis\": \"brief English summary (max 120 chars)\",\n"\
"  \"actions\": [\n"\
"    {\"cmd\": \"command_name\", \"val\": numeric_value_if_needed}\n"\
"  ]\n"\
"}\n"\
"Available commands: uv_on, uv_off, uv_auto_on, uv_auto_off, uv_dur (with val in minutes), uv_status.\n"\
"Recommend UV treatment actions to extend food preservation time based on gas concentrations.\n"\
"DO NOT include any text outside the JSON object."

#ifdef CONFIG_ELEC_NOSE_GATEWAY_ENABLE
#define UDP_SERVER_PORT CONFIG_ELEC_NOSE_UDP_PORT
#define API_KEY CONFIG_ELEC_NOSE_API_KEY
#define MODEL_API_URL CONFIG_ELEC_NOSE_MODEL_URL
#else
#define UDP_SERVER_PORT 8888
#define API_KEY ""
#define MODEL_API_URL "https://*.*.*.*/v1/chat/completions" // Censored Volcano API / 已脱敏火山引擎API
#endif

// 添加备用API URL
#define BACKUP_MODEL_API_URL "https://*.*.*.*/api/v3/chat/completions" // Censored Volcano API / 已脱敏火山引擎API

static volatile electronic_nose_gateway_state_t s_state = GATEWAY_STATE_IDLE;
static volatile bool s_running = false;
static TaskHandle_t s_task_handle = NULL;
static TaskHandle_t s_analysis_task_handle = NULL;

static gateway_state_cb_t s_state_cb = NULL;
static gateway_result_cb_t s_result_cb = NULL;

static int s_udp_socket = -1;
static bool s_wifi_connected = false;

static char s_pending_data[4096];
static size_t s_pending_data_len = 0;
static bool s_pending_data_ready = false;

static void update_state(electronic_nose_gateway_state_t new_state, const char* message)
{
    s_state = new_state;
    if (s_state_cb) {
        s_state_cb(new_state, message);
    }
    ESP_LOGI(TAG, "State: %d - %s", new_state, message ? message : "");
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        ESP_LOGI(TAG, "WiFi disconnected");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        ESP_LOGI(TAG, "WiFi got IP");
    }
}

static char s_http_response[16384];
static int s_http_response_len = 0;

static char s_analysis_prompt[4096];
static char s_analysis_response[16384];

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0) {
                int copy_len = sizeof(s_http_response) - s_http_response_len - 1;
                if (copy_len > evt->data_len) {
                    copy_len = evt->data_len;
                }
                memcpy(s_http_response + s_http_response_len, evt->data, copy_len);
                s_http_response_len += copy_len;
                s_http_response[s_http_response_len] = 0;
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            break;
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP Error");
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Manual UDP DNS resolution (ESP-Hosted getaddrinfo is broken, returns error 202)
static bool manual_dns_resolve(const char* hostname, char* ip_out, size_t ip_out_len) {
    ESP_LOGI(TAG, "manual_dns_resolve: resolving %s via UDP", hostname);

    const char *dns_servers[] = {"114.114.114.114", "8.8.8.8", "223.5.5.5", NULL};

    // Build DNS query
    uint8_t query[256];
    uint16_t tx_id = (uint16_t)(xTaskGetTickCount() & 0xFFFF);
    int qlen = 12; // header size
    memset(query, 0, qlen);
    query[0] = (uint8_t)(tx_id >> 8); query[1] = (uint8_t)(tx_id & 0xFF);
    query[2] = 0x01; query[3] = 0x00; // standard query with recursion desired
    query[4] = 0x00; query[5] = 0x01; // QDCOUNT = 1 (one question)

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
    query[qlen++] = 0; // end of name
    // Type A (1), Class IN (1)
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

        // Parse response for A record
        int rpos = 12;
        // Skip question section
        while (rpos < rlen && resp[rpos] != 0) {
            if (resp[rpos] >= 0xC0) { rpos += 2; break; }
            rpos += resp[rpos] + 1;
        }
        if (rpos < rlen && resp[rpos] == 0) rpos++;
        rpos += 4; // skip QTYPE + QCLASS

        while (rpos + 12 <= rlen) {
            // Skip name (may be compressed pointer)
            if (resp[rpos] >= 0xC0) { rpos += 2; }
            else { while (rpos < rlen && resp[rpos] != 0) rpos += resp[rpos] + 1; if (rpos < rlen) rpos++; }

            if (rpos + 10 > rlen) break;
            uint16_t rtype = (resp[rpos] << 8) | resp[rpos+1];
            rpos += 8; // type(2) + class(2) + ttl(4)
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

// Extract hostname from URL and resolve to IP
static bool resolve_url_to_ip(const char* url, char* resolved_url, size_t resolved_url_len) {
    const char *proto_end = strstr(url, "://");
    if (!proto_end) return false;
    proto_end += 3;

    const char *path_start = strchr(proto_end, '/');
    const char *port_start = strchr(proto_end, ':');

    char hostname[128] = {0};
    size_t hlen;

    if (port_start && (!path_start || port_start < path_start)) {
        hlen = port_start - proto_end;
    } else if (path_start) {
        hlen = path_start - proto_end;
    } else {
        hlen = strlen(proto_end);
    }
    if (hlen >= sizeof(hostname)) hlen = sizeof(hostname) - 1;
    memcpy(hostname, proto_end, hlen);

    char ip[48] = {0};
    if (!manual_dns_resolve(hostname, ip, sizeof(ip))) return false;

    ESP_LOGI(TAG, "URL resolved: %s -> %s", hostname, ip);

    // Rebuild URL: proto://ip[:port]/path
    int prefix_len = (proto_end - url);
    const char *after_host = path_start ? path_start : (proto_end + hlen);
    snprintf(resolved_url, resolved_url_len, "%.*s%s%s",
             prefix_len, url, ip, after_host);
    return true;
}

static esp_err_t call_edge_gateway_api_with_url(const char* url, const char* json_body, char* response, size_t response_size, int* response_len)
{
    // Extract original hostname from URL
    char hostname[128] = {0};
    const char *proto = strstr(url, "://");
    if (proto) {
        const char *start = proto + 3;
        const char *end = strchr(start, '/');
        const char *port = strchr(start, ':');
        size_t hlen;
        if (port && (!end || port < end)) {
            hlen = port - start;
        } else if (end) {
            hlen = end - start;
        } else {
            hlen = strlen(start);
        }
        if (hlen >= sizeof(hostname)) hlen = sizeof(hostname) - 1;
        memcpy(hostname, start, hlen);
    }

    // Manual DNS: resolve hostname to IP to bypass broken getaddrinfo
    char resolved_url[512] = {0};
    const char *target_url = url;
    bool using_manual_dns = false;

    if (hostname[0] != '\0' && resolve_url_to_ip(url, resolved_url, sizeof(resolved_url))) {
        ESP_LOGI(TAG, "Manual DNS resolved %s -> %s", hostname, resolved_url);
        target_url = resolved_url;
        using_manual_dns = true;
    } else {
        ESP_LOGW(TAG, "Manual DNS failed, fallback to getaddrinfo (may fail on this platform)");
    }

    esp_http_client_config_t config = {
        .url = target_url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = using_manual_dns,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for URL: %s", url);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (using_manual_dns) {
        esp_http_client_set_header(client, "Host", hostname);
    }
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", API_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header);

    int body_len = strlen(json_body);
    esp_http_client_set_post_field(client, json_body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    if (s_http_response_len > 0) {
        strlcpy(response, s_http_response, response_size);
        *response_len = strlen(response);
    }

    esp_http_client_cleanup(client);
    return (err == ESP_OK && status_code == 200) ? ESP_OK : ESP_FAIL;
}

static esp_err_t call_edge_gateway_api(const char* prompt, char* response, size_t response_size, int* response_len)
{
    if (!s_wifi_connected) {
        ESP_LOGE(TAG, "WiFi not connected");
        return ESP_FAIL;
    }

    if (response == NULL || response_len == NULL || response_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s_http_response, 0, sizeof(s_http_response));
    s_http_response_len = 0;
    response[0] = 0;
    *response_len = 0;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "model", "doubao-seed-1.6");

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", SYSTEM_PROMPT);
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", prompt);
    cJSON_AddItemToArray(messages, user_msg);

    cJSON_AddNumberToObject(root, "max_tokens", 512);

    char *json_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_body) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Using API Key: %s", API_KEY);

    // 定义要尝试的URL列表
    const char* urls[] = {MODEL_API_URL, BACKUP_MODEL_API_URL};
    int num_urls = sizeof(urls) / sizeof(urls[0]);
    
    esp_err_t result = ESP_FAIL;
    
    // 尝试每一个URL
    for (int i = 0; i < num_urls; i++) {
        ESP_LOGI(TAG, "Trying API URL [%d]: %s", i+1, urls[i]);
        
        result = call_edge_gateway_api_with_url(urls[i], json_body, response, response_size, response_len);
        
        int status = (result == ESP_OK) ? 200 : 0;
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "API call successful with URL: %s", urls[i]);
            break;  // 成功就退出循环
        } else {
            ESP_LOGW(TAG, "API call failed with URL [%d]: %s", i+1, urls[i]);
            
            // 清除上一次的响应缓存
            memset(s_http_response, 0, sizeof(s_http_response));
            s_http_response_len = 0;
            response[0] = 0;
            *response_len = 0;
            
            // 如果不是最后一次尝试，稍微等待一下再试下一个
            if (i < num_urls - 1) {
                vTaskDelay(pdMS_TO_TICKS(1000));  // 等待1秒再试下一个URL
            }
        }
    }

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "All API calls failed");
    } else {
        int status = 200;
        ESP_LOGI(TAG, "HTTP Status = %d", status);
        if (s_http_response_len > 0) {
            ESP_LOGI(TAG, "Response body: %s", s_http_response);
        }
    }

    free(json_body);
    return result;
}

static void analysis_task(void* param)
{
    char* data = (char*)param;
    ESP_LOGI(TAG, "Analyzing sensor data...");

    update_state(GATEWAY_STATE_PROCESSING, "AI analyzing...");

    memset(s_analysis_prompt, 0, sizeof(s_analysis_prompt));
    memset(s_analysis_response, 0, sizeof(s_analysis_response));

    snprintf(s_analysis_prompt, sizeof(s_analysis_prompt),
        "Cold storage sensor data: %s. "
        "Assess quality and recommend UV preservation actions.",
        data);

    int response_len = 0;
    esp_err_t err = call_edge_gateway_api(s_analysis_prompt, s_analysis_response, sizeof(s_analysis_response), &response_len);

    if (err == ESP_OK && response_len > 0) {
        cJSON *root = cJSON_Parse(s_analysis_response);
        if (root) {
            cJSON *choices = cJSON_GetObjectItem(root, "choices");
            if (choices && cJSON_GetArraySize(choices) > 0) {
                cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
                cJSON *message = cJSON_GetObjectItem(first_choice, "message");
                cJSON *content = cJSON_GetObjectItem(message, "content");
                if (content && content->valuestring && content->valuestring[0]) {
                    ESP_LOGI(TAG, "Analysis result: %s", content->valuestring);
                    if (s_result_cb) {
                        s_result_cb(content->valuestring);
                    }
                    update_state(GATEWAY_STATE_CONNECTED, "Analysis complete");
                } else {
                    ESP_LOGE(TAG, "API response missing content field");
                    update_state(GATEWAY_STATE_ERROR, "API response error");
                }
            } else {
                ESP_LOGE(TAG, "API response has no choices");
                update_state(GATEWAY_STATE_ERROR, "API response error");
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGE(TAG, "Failed to parse API response JSON (buffer too small?)");
            update_state(GATEWAY_STATE_ERROR, "Response parse failed");
        }
    } else {
        ESP_LOGE(TAG, "Analysis failed");
        update_state(GATEWAY_STATE_ERROR, "Analysis failed");
    }

    free(data);
    s_analysis_task_handle = NULL;
    vTaskDelete(NULL);
}

static void udp_server_task(void* param)
{
    char rx_buffer[1024];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_SERVER_PORT);

    s_udp_socket = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (s_udp_socket < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        update_state(GATEWAY_STATE_ERROR, "Socket creation failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_udp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGI(TAG, "UDP server socket created");

    if (bind(s_udp_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Socket bind failed");
        close(s_udp_socket);
        s_udp_socket = -1;
        update_state(GATEWAY_STATE_ERROR, "Socket bind failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP server bound to port %d", UDP_SERVER_PORT);
    update_state(GATEWAY_STATE_CONNECTED, "UDP Server started");

    while (s_running) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(s_udp_socket, rx_buffer, sizeof(rx_buffer) - 1, 0,
                          (struct sockaddr *)&source_addr, &socklen);

        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom error");
            break;
        } else {
            rx_buffer[len] = 0;
            inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str));
            ESP_LOGI(TAG, "Received %d bytes from %s: %s", len, addr_str, rx_buffer);

            update_state(GATEWAY_STATE_RECEIVING, "Data received");

            if (s_analysis_task_handle == NULL) {
                char* data_copy = malloc(len + 1);
                if (data_copy) {
                    memcpy(data_copy, rx_buffer, len);
                    data_copy[len] = 0;

                    xTaskCreate(&analysis_task, "analysis_task", 8192, data_copy, 5, &s_analysis_task_handle);
                }
            } else {
                ESP_LOGW(TAG, "Analysis in progress, data dropped");
            }
        }
    }

    if (s_udp_socket >= 0) {
        close(s_udp_socket);
        s_udp_socket = -1;
    }
    vTaskDelete(NULL);
}

esp_err_t electronic_nose_gateway_init(void)
{
    ESP_LOGI(TAG, "Initializing Electronic Nose Gateway...");

    s_wifi_connected = false;
    s_pending_data_len = 0;
    s_pending_data_ready = false;

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    update_state(GATEWAY_STATE_IDLE, "Initialized");
    return ESP_OK;
}

esp_err_t electronic_nose_gateway_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    s_running = true;
    xTaskCreate(&udp_server_task, "udp_server", 4096, NULL, 5, &s_task_handle);

    return ESP_OK;
}

esp_err_t electronic_nose_gateway_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }

    s_running = false;

    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }

    if (s_udp_socket >= 0) {
        close(s_udp_socket);
        s_udp_socket = -1;
    }

    update_state(GATEWAY_STATE_IDLE, "Stopped");
    return ESP_OK;
}

esp_err_t electronic_nose_gateway_deinit(void)
{
    electronic_nose_gateway_stop();
    s_wifi_connected = false;
    return ESP_OK;
}

esp_err_t electronic_nose_gateway_set_state_callback(gateway_state_cb_t callback)
{
    s_state_cb = callback;
    return ESP_OK;
}

esp_err_t electronic_nose_gateway_set_result_callback(gateway_result_cb_t callback)
{
    s_result_cb = callback;
    return ESP_OK;
}

electronic_nose_gateway_state_t electronic_nose_gateway_get_state(void)
{
    return s_state;
}

bool electronic_nose_gateway_is_connected(void)
{
    return s_wifi_connected;
}

esp_err_t electronic_nose_gateway_trigger_analysis(const char* sensor_data, size_t len)
{
    if (!sensor_data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_wifi_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char* data_copy = malloc(len + 1);
    if (!data_copy) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(data_copy, sensor_data, len);
    data_copy[len] = 0;

    if (s_analysis_task_handle == NULL) {
        xTaskCreate(&analysis_task, "analysis_task", 8192, data_copy, 5, &s_analysis_task_handle);
        return ESP_OK;
    } else {
        free(data_copy);
        return ESP_ERR_INVALID_STATE;
    }
}
