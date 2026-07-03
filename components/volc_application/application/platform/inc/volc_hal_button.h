// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#ifndef __VOLC_HAL_BUTTON_H__
#define __VOLC_HAL_BUTTON_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef void* volc_hal_button_t;

typedef enum {
    VOLC_BUTTON_PRESS_DOWN = 0, // Button press down event
    VOLC_BUTTON_PRESS_UP,   // Button press up event
    VOLC_BUTTON_PRESS_REPEAT, // Button press repeat event
    VOLC_BUTTON_PRESS_REPEAT_DONE, // Button press repeat done event
    VOLC_BUTTON_SINGLE_CLICK, // Button single click event
    VOLC_BUTTON_DOUBLE_CLICK, // Button double click event
    VOLC_BUTTON_MULTIPLE_CLICK, // Button multiple click event
    VOLC_BUTTON_LONG_PRESS_START, // Button long press start event
    VOLC_BUTTON_LONG_PRESS_HOLD, // Button long press hold event
    VOLC_BUTTON_LONG_PRESS_UP, // Button long press up event
    VOLC_BUTTON_PRESS_END, // Button press end event
    VOLC_BUTTON_EVENT_MAX, // Maximum number of button events
} volc_hal_button_event_e;

typedef void (*volc_hal_button_event_cb_t)(volc_hal_button_t button, volc_hal_button_event_e event, void* user_data);

typedef struct volc_hal_button_config {
    int gpio_num;                             // GPIO number of the button
    int active_level;                         // Active level of the button (0 or 1)
    int long_press_ms;                        // Long press threshold in milliseconds
    int short_press_ms;                       // Short press threshold in milliseconds
    int enable_power_save;                    // Whether to enable power save mode (0 or 1)
    void* user_data;                           // User data pointer
    volc_hal_button_event_cb_t event_cb;         // Button event callback function
} volc_hal_button_config_t;

/**
 * @brief Create a button instance
 * 
 * @param config Button configuration
 * @return volc_hal_button_t Button instance pointer
 */
volc_hal_button_t volc_hal_button_create(volc_hal_button_config_t* config);

/**
 * @brief Destroy a button instance
 * 
 * @param button Button instance pointer
 */
void volc_hal_button_destroy(volc_hal_button_t button);

#ifdef __cplusplus
}
#endif

#endif
