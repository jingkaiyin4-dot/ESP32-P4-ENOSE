/*
 * app_system.c - System-wide event group and PPA manager implementation
 */

#include "app_system.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"

static const char *TAG = "APP_SYSTEM";

/* ============================================================
 * System Event Group
 * ============================================================ */
static EventGroupHandle_t s_system_eg = NULL;

void app_system_event_group_init(void)
{
    if (s_system_eg == NULL) {
        s_system_eg = xEventGroupCreate();
        ESP_LOGI(TAG, "System event group created");
    }
}

EventGroupHandle_t app_system_event_group(void)
{
    return s_system_eg;
}

/* ============================================================
 * PPA Manager
 * ============================================================ */
static ppa_client_handle_t s_ppa_web_client = NULL;
static ppa_client_handle_t s_ppa_qr_client = NULL;

esp_err_t app_ppa_init(void)
{
    esp_err_t ret;

    /* Register PPA SRM client for Web JPEG pipeline (RGB565→RGB888 + downscale) */
    ppa_client_config_t web_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ret = ppa_register_client(&web_cfg, &s_ppa_web_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA web SRM client: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "PPA web SRM client registered");

    /* Register PPA SRM client for QR gray pipeline (RGB565→GRAY8 + downscale) */
    ppa_client_config_t qr_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ret = ppa_register_client(&qr_cfg, &s_ppa_qr_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA QR SRM client: %s", esp_err_to_name(ret));
        ppa_unregister_client(s_ppa_web_client);
        s_ppa_web_client = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "PPA QR SRM client registered");

    /* Set RGB→Gray formula: ITU-R BT.601 luma coefficients (77, 150, 29 sum=256) */
    ret = ppa_set_rgb2gray_formula(77, 150, 29);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ppa_set_rgb2gray_formula failed: %s (gray conversion may use default)", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "PPA initialized - hardware acceleration active");
    return ESP_OK;
}

ppa_client_handle_t app_ppa_get_web_client(void)
{
    return s_ppa_web_client;
}

ppa_client_handle_t app_ppa_get_qr_client(void)
{
    return s_ppa_qr_client;
}

esp_err_t app_ppa_deinit(void)
{
    if (s_ppa_web_client) {
        ppa_unregister_client(s_ppa_web_client);
        s_ppa_web_client = NULL;
    }
    if (s_ppa_qr_client) {
        ppa_unregister_client(s_ppa_qr_client);
        s_ppa_qr_client = NULL;
    }
    ESP_LOGI(TAG, "PPA deinitialized");
    return ESP_OK;
}
