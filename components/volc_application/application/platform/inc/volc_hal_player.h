// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#ifndef __VOLC_HAL_PLAYER_H__
#define __VOLC_HAL_PLAYER_H__

#include "volc_hal_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Player instance pointer
 */
typedef void* volc_hal_player_t;

typedef struct volc_hal_player_config {
    volc_media_type_e media_type;   // Media type
} volc_hal_player_config_t;

/**
 * @brief Create a player instance
 * @details this function shoudle set the player_handle value in the volc_hal_context_t structure.
 * @note We do not specify specific media configurations (e.g., encoding format, sampling rate, or resolution).
 *      This is because we expect all the aforementioned parameters to be uniquely determined and immutable across all scenarios.
 *      Therefore, these values must be fixed at compile time; if your device supports multiple parameters,
 *      macros should be used to select the desired ones at compile time.
 * 
 * @param config Player configuration
 * @return volc_hal_player_t Player instance pointer
 */
volc_hal_player_t volc_hal_player_create(volc_hal_player_config_t* config);

/**
 * @brief set audio player volume
 * 
 * @param player Player instance pointer (audio)
 * @param volume the volume [0,100]
 * @return int 0 if success 
 */
int volc_hal_set_audio_player_volume(volc_hal_player_t player, int volume);

/**
 * @brief get audio player volume
 * 
 * @param player Player instance pointer (audio)
 * @return the volume
 */
int volc_hal_get_audio_player_volume(volc_hal_player_t player);

/**
 * @brief Destroy a player instance
 * @details this function shoudle set the player_handle to NULL in the volc_hal_context_t structure.
 * 
 * @param player Player instance pointer
 */
void volc_hal_player_destroy(volc_hal_player_t player);

/**
 * @brief Start a player instance
 * 
 * @param player Player instance pointer
 * @return int 0 if success, otherwise error code
 */
int volc_hal_player_start(volc_hal_player_t player);

/**
 * @brief Stop a player instance
 * 
 * @param player Player instance pointer
 * @return int 0 if success, otherwise error code
 */
int volc_hal_player_stop(volc_hal_player_t player);

/**
 * @brief Play data with a player instance
 * @note We have not specified whether this function is sync or async.
 *       However, it is strongly recommended to implement this function as non-blocking
 *       with a certain internal buffer, ensuring that the caller of the function
 *       does not need to overly consider the function's call frequency.    
 *       For example, the function can be called every 1 second to play 1.2 seconds of audio or video data.
 * 
 * @param player Player instance pointer
 * @param data Data to play
 * @param len Data length
 * @return int 0 if success, otherwise error code
 */
int volc_hal_player_play_data(volc_hal_player_t player, const void* data, int len);

#ifdef __cplusplus
}
#endif

#endif
