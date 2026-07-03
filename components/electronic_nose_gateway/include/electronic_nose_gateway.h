#ifndef ELECTRONIC_NOSE_GATEWAY_H
#define ELECTRONIC_NOSE_GATEWAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GATEWAY_STATE_IDLE = 0,
    GATEWAY_STATE_CONNECTED,
    GATEWAY_STATE_RECEIVING,
    GATEWAY_STATE_PROCESSING,
    GATEWAY_STATE_ERROR
} electronic_nose_gateway_state_t;

typedef void (*gateway_state_cb_t)(electronic_nose_gateway_state_t state, const char* message);
typedef void (*gateway_result_cb_t)(const char* result);

esp_err_t electronic_nose_gateway_init(void);
esp_err_t electronic_nose_gateway_start(void);
esp_err_t electronic_nose_gateway_stop(void);
esp_err_t electronic_nose_gateway_deinit(void);

esp_err_t electronic_nose_gateway_set_state_callback(gateway_state_cb_t callback);
esp_err_t electronic_nose_gateway_set_result_callback(gateway_result_cb_t callback);

electronic_nose_gateway_state_t electronic_nose_gateway_get_state(void);
bool electronic_nose_gateway_is_connected(void);

esp_err_t electronic_nose_gateway_trigger_analysis(const char* sensor_data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
