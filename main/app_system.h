/*
 * app_system.h - System-wide event group, core affinity, and PPA manager
 *
 * Centralizes RTOS primitives for the E-Nose project:
 *   - FreeRTOS EventGroup for system state (replaces polling timers)
 *   - Core affinity definitions (Core 0: real-time DMA/PPA/I2S; Core 1: LVGL/WebSocket/BLE)
 *   - PPA client handles for zero-copy video pipeline
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "driver/ppa.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * System Event Group - replaces polling with event-driven sync
 * ============================================================ */
#define APP_EVENT_WIFI_CONNECTED   BIT(0)
#define APP_EVENT_WIFI_DISCONNECTED BIT(1)
#define APP_EVENT_WIFI_SCAN_DONE   BIT(2)
#define APP_EVENT_BLE_SYNCED       BIT(3)
#define APP_EVENT_AI_READY         BIT(4)
#define APP_EVENT_CAMERA_READY     BIT(5)
#define APP_EVENT_GATEWAY_READY    BIT(6)

/**
 * @brief Get the global system event group (created once in app_main)
 */
EventGroupHandle_t app_system_event_group(void);

/**
 * @brief Initialize the system event group
 */
void app_system_event_group_init(void);

/* ============================================================
 * Core Affinity - strict dual-core responsibility division
 *
 * Core 0 (PRO_CPU): Real-time DMA, PPA hardware, I2S audio,
 *                   camera V4L2, video stream task
 * Core 1 (APP_CPU): LVGL rendering, WebSocket/cJSON, BLE host,
 *                    HTTP server, sensor data processing
 * ============================================================ */
#define APP_CORE_REALTIME   0   // Core 0: DMA, PPA, I2S, camera
#define APP_CORE_APP        1   // Core 1: LVGL, WebSocket, BLE, HTTP

/* ============================================================
 * PPA Manager - centralized PPA client registration
 * ============================================================ */

/**
 * @brief Initialize PPA clients for SRM operations
 *
 * Registers two PPA SRM clients:
 *   - Web JPEG pipeline: RGB565 → RGB888 + downscale
 *   - QR gray pipeline: RGB565 → GRAY8 + downscale
 */
esp_err_t app_ppa_init(void);

/**
 * @brief Get PPA SRM client for web JPEG pipeline
 */
ppa_client_handle_t app_ppa_get_web_client(void);

/**
 * @brief Get PPA SRM client for QR gray pipeline
 */
ppa_client_handle_t app_ppa_get_qr_client(void);

/**
 * @brief Deinitialize PPA clients
 */
esp_err_t app_ppa_deinit(void);

#ifdef __cplusplus
}
#endif
