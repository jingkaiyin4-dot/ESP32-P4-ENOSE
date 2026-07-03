// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#ifndef __VOLC_HAL_H__
#define __VOLC_HAL_H__

#include "volc_hal_button.h"
#include "volc_hal_capture.h"
#include "volc_hal_display.h"
#include "volc_hal_player.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOLC_DEVICE_NAME_LENGTH 32
typedef enum {
    VOLC_HAL_CAPTURE_AUDIO = 0,  // audio capture
    VOLC_HAL_CAPTURE_VIDEO,      // video capture
    VOLC_HAL_CAPTURE_MAX,        // Maximum capture count
} volc_hal_capture_e;

typedef enum {
    VOLC_HAL_PLAYER_AUDIO = 0,  // audio player
    VOLC_HAL_PLAYER_VIDEO,      // video player
    VOLC_HAL_PLAYER_MAX,        // Maximum player count
} volc_hal_player_e;

/**
 * @brief All hardware resources handle and context
 * @details generally,this structure should be singleton
 *          you can get the Instance by volc_get_global_hal_context()
 *          And, you just set the value when the hardware submodule be inited
 *          the function volc_hal_init() just do some necessary initialization actions
 *          such as, malloc the volc_hal_context_t mem and set the device_name
 */
typedef struct {
    /**
     * @brief the button handle pointer
     * @details It should be set value when the volc_hal_button_create() is called
     */
    volc_hal_button_t button_handle;
    /**
     * @brief the capture pointer array, include audio_capture and video_capture
     * @details  It should be set value when the volc_hal_capture_create() is called
     */
    volc_hal_capture_t capture_handle[VOLC_HAL_CAPTURE_MAX];
    /**
     * @brief the display pointer
     * @details It should be set value when the volc_hal_display_create() is called
     */
    volc_hal_display_t display_handle;
    /**
     * @brief the player pointer array, include audio_player and video_player 
     * @details It should be set value when the volc_hal_player_create() is called
     */
    volc_hal_player_t player_handle[VOLC_HAL_PLAYER_MAX];
    /**
     * @brief the device name, in most cases, it is a mac address
     * @details It should be set a value when the volc_hal_init() is called
     */    
    char device_name[VOLC_DEVICE_NAME_LENGTH];
} volc_hal_context_t;

/**
 * @brief Initialize the HAL module
 * @details Do the necessary actions after the hardware is powered on
 *          it is not necessary to initialize all the hardware modules
 *          but it is necessary to set the device name 
 * 
 * @return int 0 if success, otherwise error code
 */
int volc_hal_init(void);

/**
 * @brief return global hal context
 * @details you can get the singleton Instance  volc_hal_context_t by volc_get_global_hal_context()
 *          this function could help you get the hal context everywhere
 *          please be carefully, Do not Modify the return value
 */
volc_hal_context_t* volc_get_global_hal_context();

/**
 * @brief Deinitialize the HAL module
 */
void volc_hal_fini(void);

#ifdef __cplusplus
}
#endif

#endif