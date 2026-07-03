#pragma once

#include <time.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "uart_receiver.h"

#define CSV_MAX_RECORDS 600
#define CSV_BATCH_INTERVAL_MS 600000 // 10分钟 = 600,000ms

struct CsvRecord {
    time_t timestamp;
    float odor;
    float hcho;
    float co;
    float voc;
    int co2;
    float temp;
    float humidity;
    char cls[32];
    float conf;
    int fr;
    int uv_on;
};

static CsvRecord *s_csv_records = NULL;
static int s_csv_head = 0;
static int s_csv_count = 0;
static SemaphoreHandle_t s_csv_mutex = NULL;
static uint32_t s_last_flush_time = 0;

static const char *CSV_TAG = "CSV_BUF";

inline void csv_buffer_init(void)
{
    s_csv_mutex = xSemaphoreCreateMutex();
    if (!s_csv_mutex) {
        ESP_LOGE(CSV_TAG, "Failed to create CSV mutex");
        return;
    }
    
    // 从 PSRAM 中分配缓冲数组，绝对不占用内部 SRAM
    s_csv_records = (CsvRecord *)heap_caps_malloc(sizeof(CsvRecord) * CSV_MAX_RECORDS, MALLOC_CAP_SPIRAM);
    if (!s_csv_records) {
        ESP_LOGE(CSV_TAG, "Failed to allocate CSV buffer in PSRAM");
        return;
    }
    
    s_csv_head = 0;
    s_csv_count = 0;
    s_last_flush_time = xTaskGetTickCount();
    ESP_LOGI(CSV_TAG, "CSV buffer initialized in PSRAM successfully (600 records)");
}

inline void csv_buffer_append(const ble_sensor_data_t *data)
{
    if (!s_csv_records || !s_csv_mutex) return;
    
    if (xSemaphoreTake(s_csv_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(CSV_TAG, "Failed to acquire lock to append CSV record");
        return;
    }
    
    CsvRecord rec;
    rec.timestamp = time(NULL);
    rec.odor = data->odor;
    rec.hcho = data->hcho;
    rec.co = data->co;
    rec.voc = data->voc;
    rec.co2 = data->co2;
    rec.temp = data->temp;
    rec.humidity = data->hum;
    strncpy(rec.cls, data->sensor_class, sizeof(rec.cls) - 1);
    rec.cls[sizeof(rec.cls) - 1] = '\0';
    rec.conf = data->conf;
    rec.fr = data->fresh;
    rec.uv_on = data->uv ? 1 : 0;
    
    s_csv_records[s_csv_head] = rec;
    s_csv_head = (s_csv_head + 1) % CSV_MAX_RECORDS;
    if (s_csv_count < CSV_MAX_RECORDS) {
        s_csv_count++;
    }
    
    xSemaphoreGive(s_csv_mutex);
    ESP_LOGD(CSV_TAG, "Appended sensor record. count=%d, head=%d", s_csv_count, s_csv_head);
}

inline void csv_buffer_flush_to_server(void)
{
    if (!s_csv_records || !s_csv_mutex || s_csv_count == 0) return;
    
    if (xSemaphoreTake(s_csv_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(CSV_TAG, "Failed to lock CSV buffer for flushing");
        return;
    }
    
    // 1. 在 PSRAM 中分配组装缓冲区 (80KB)
    char *csv_buf = (char *)heap_caps_malloc(80 * 1024, MALLOC_CAP_SPIRAM);
    if (!csv_buf) {
        ESP_LOGE(CSV_TAG, "Failed to allocate build buffer in PSRAM");
        xSemaphoreGive(s_csv_mutex);
        return;
    }
    
    // 2. 组装 CSV 头 (格式必须与 SD 卡日志完美一致)
    int offset = snprintf(csv_buf, 80 * 1024, "timestamp,odor,hcho,co,voc,co2,temp,humidity,cls,conf,fr,uv_on\n");
    
    // 3. 确定起始索引并遍历
    int start_idx = 0;
    if (s_csv_count == CSV_MAX_RECORDS) {
        start_idx = s_csv_head;
    }
    
    for (int i = 0; i < s_csv_count; i++) {
        int idx = (start_idx + i) % CSV_MAX_RECORDS;
        const CsvRecord &rec = s_csv_records[idx];
        
        struct tm tm_info;
        localtime_r(&rec.timestamp, &tm_info);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_info);
        
        int written = snprintf(csv_buf + offset, (80 * 1024) - offset,
                               "%s,%.2f,%.2f,%.1f,%.1f,%d,%.1f,%.1f,%s,%.2f,%d,%d\n",
                               time_str, rec.odor, rec.hcho, rec.co, rec.voc, rec.co2,
                               rec.temp, rec.humidity, rec.cls, rec.conf, rec.fr, rec.uv_on);
        if (written > 0 && offset + written < 80 * 1024 - 100) {
            offset += written;
        } else {
            break; // 缓冲区已满
        }
    }
    
    ESP_LOGI(CSV_TAG, "CSV Payload assembled. Length: %d bytes, records: %d", offset, s_csv_count);
    
    // 4. 构建上传参数 (获取当前本地时间用于 URL 属性参数)
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char date_str[16];
    char hour_str[4];
    char min_str[4];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm_now);
    strftime(hour_str, sizeof(hour_str), "%H", &tm_now);
    strftime(min_str, sizeof(min_str), "%M", &tm_now);
    
    char url[256];
    snprintf(url, sizeof(url), "http://*.*.*.*/api/sensor/csv?key=***&date=%s&hour=%s&minute=%s", // Censored Server API / 已脱敏服务器API
             date_str, hour_str, min_str);
             
    // 5. 释放互斥锁，因为 HTTP 上传是阻塞操作，我们先不持有互斥锁以防止阻塞数据接收 append 动作
    xSemaphoreGive(s_csv_mutex);
    
    // 6. 执行 HTTP POST 上传
    bool success = false;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 15000;
    
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_header(client, "Content-Type", "text/csv");
        esp_http_client_set_post_field(client, csv_buf, offset);
        
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            if (status == 200) {
                ESP_LOGI(CSV_TAG, "CSV uploaded successfully. Status: 200");
                success = true;
            } else {
                ESP_LOGE(CSV_TAG, "CSV upload failed. HTTP status code: %d", status);
            }
        } else {
            ESP_LOGE(CSV_TAG, "HTTP POST perform failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    } else {
        ESP_LOGE(CSV_TAG, "Failed to init HTTP client");
    }
    
    heap_caps_free(csv_buf);
    
    // 7. 处理队列：如果上传成功，清空已发送数据，仅保留最新 10 条防网络抖动丢包
    if (success) {
        if (xSemaphoreTake(s_csv_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            int current_count = s_csv_count;
            int keep_num = (current_count > 10) ? 10 : current_count;
            
            CsvRecord temp_records[10];
            int start_idx = 0;
            if (current_count == CSV_MAX_RECORDS) {
                start_idx = s_csv_head;
            }
            
            int copy_start = current_count - keep_num;
            for (int i = 0; i < keep_num; i++) {
                int idx = (start_idx + copy_start + i) % CSV_MAX_RECORDS;
                temp_records[i] = s_csv_records[idx];
            }
            
            for (int i = 0; i < keep_num; i++) {
                s_csv_records[i] = temp_records[i];
            }
            
            s_csv_head = keep_num;
            s_csv_count = keep_num;
            s_last_flush_time = xTaskGetTickCount();
            
            xSemaphoreGive(s_csv_mutex);
            ESP_LOGI(CSV_TAG, "CSV buffer flushed. Remaining %d records for backup", s_csv_count);
        }
    }
}

inline void csv_buffer_check_and_flush(void)
{
    uint32_t now = xTaskGetTickCount();
    if (now - s_last_flush_time >= pdMS_TO_TICKS(CSV_BATCH_INTERVAL_MS)) {
        s_last_flush_time = now; // 立即更新时间基准，防止因上传失败导致高频无限重试
        ESP_LOGI(CSV_TAG, "10-minute boundary reached. Starting auto CSV flush...");
        csv_buffer_flush_to_server();
    }
}
