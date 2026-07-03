/*
 * XiaoZhi AI Service - ESP32-P4 Port
 * Based on 78/xiaozhi-esp32 implementation
 */

#ifndef __XIAOZHI_AI_SERVICE_H__
#define __XIAOZHI_AI_SERVICE_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XIAOZHI_STATE_IDLE = 0,
    XIAOZHI_STATE_CONNECTING,
    XIAOZHI_STATE_CONNECTED,
    XIAOZHI_STATE_ACTIVATING,  // Obtaining activation code
    XIAOZHI_STATE_LISTENING,
    XIAOZHI_STATE_THINKING,
    XIAOZHI_STATE_SPEAKING,
    XIAOZHI_STATE_ERROR
} xiaozhi_state_t;

typedef void (*xiaozhi_state_cb_t)(xiaozhi_state_t state, const char* message);
typedef void (*xiaozhi_text_cb_t)(const char* text);

/**
 * @brief Initialize the XiaoZhi AI service
 */
esp_err_t xiaozhi_ai_service_init(void);

/**
 * @brief Start the service (connect to Wi-Fi first)
 */
esp_err_t xiaozhi_ai_service_start(void);

/**
 * @brief Stop the service
 */
esp_err_t xiaozhi_ai_service_stop(void);

/**
 * @brief Set PTT state
 */
void xiaozhi_ai_service_set_ptt(bool press);

/**
 * @brief Set callbacks for state changes and text/subtitle updates
 */
esp_err_t xiaozhi_ai_service_set_state_callback(xiaozhi_state_cb_t callback);
esp_err_t xiaozhi_ai_service_set_text_callback(xiaozhi_text_cb_t callback);

/**
 * @brief Get the current state
 */
xiaozhi_state_t xiaozhi_ai_service_get_state(void);

/**
 * @brief Check if the service is connected to the backend
 */
bool xiaozhi_ai_service_is_connected(void);

/**
 * @brief Send text message to trigger cloud speech synthesis (TTS)
 */
void xiaozhi_ai_service_send_text(const char* text);
void xiaoZhi_tts_broadcast(const char* text);

#ifdef __cplusplus
}
#endif

#endif // __XIAOZHI_AI_SERVICE_H__
