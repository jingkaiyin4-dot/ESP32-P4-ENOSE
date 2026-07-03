/*
 * Volcano RTC Service - Simplified Integration
 * Uses existing bsp_extra_codec for audio
 * Direct WebSocket/REST API communication
 */

#ifndef __VOLCANO_RTC_SERVICE_H__
#define __VOLCANO_RTC_SERVICE_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOLCANO_RTC_STATE_IDLE = 0,
    VOLCANO_RTC_STATE_CONNECTING,
    VOLCANO_RTC_STATE_CONNECTED,
    VOLCANO_RTC_STATE_RECORDING,
    VOLCANO_RTC_STATE_PLAYING,
    VOLCANO_RTC_STATE_ERROR
} volcano_rtc_state_t;

typedef void (*volcano_rtc_state_cb_t)(volcano_rtc_state_t state, const char* message);
typedef void (*volcano_rtc_text_cb_t)(const char* text);

esp_err_t volcano_rtc_service_init(void);
esp_err_t volcano_rtc_service_start(void);
esp_err_t volcano_rtc_service_stop(void);
esp_err_t volcano_rtc_service_set_state_callback(volcano_rtc_state_cb_t callback);
esp_err_t volcano_rtc_service_set_text_callback(volcano_rtc_text_cb_t callback);
volcano_rtc_state_t volcano_rtc_service_get_state(void);
bool volcano_rtc_service_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif
