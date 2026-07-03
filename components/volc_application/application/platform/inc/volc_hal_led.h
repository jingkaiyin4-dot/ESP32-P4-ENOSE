// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#ifndef __VOLC_HAL_LED_H__
#define __VOLC_HAL_LED_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED instance pointer
 */
typedef void* volc_led_t;

/**
 * @brief LED state enumeration
 */
typedef enum {
    VOLC_LED_OFF = 0, // LED off state
    VOLC_LED_ON,      // LED on state
} volc_led_state_e;

typedef struct volc_led_config {
    int gpio_num;          // GPIO number of the LED
    int active_level;      // Active level of the LED (0 or 1)
} volc_led_config_t;

/**
 * @brief Create a LED instance
 * 
 * @param config LED configuration
 * @return volc_led_t LED instance pointer
 */
volc_led_t volc_led_create(volc_led_config_t* config);

/**
 * @brief Destroy a LED instance
 * 
 * @param led LED instance pointer
 */
void volc_led_destroy(volc_led_t led);

/**
 * @brief Set the LED state
 * 
 * @param led LED instance pointer
 * @param state LED state (VOLC_LED_OFF, VOLC_LED_ON, or VOLC_LED_BLINK)
 */
void volc_led_set_state(volc_led_t led, volc_led_state_e state);

#ifdef __cplusplus
}
#endif

#endif