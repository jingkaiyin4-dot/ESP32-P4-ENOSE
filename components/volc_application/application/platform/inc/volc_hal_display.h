// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#ifndef __VOLC_HAL_DISPLAY_H__
#define __VOLC_HAL_DISPLAY_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Display instance pointer
 */
typedef void* volc_hal_display_t;

/** This enumeration exists to map to distinct regions of the screen.
 * Note: Typically, a set of screen display interfaces allows upper-layer callers
 * to independently create and destroy regions for display purposes.
 * However, to minimize the development effort of upper-layer business logic,
 * we do not allow the business side to create and manage different screen regions on their own.
 * Instead, different screen regions are divided and created during the execution of volc_hal_display_create().
 * The business layer can only obtain different render layer objects,
 * pass data to them, and perform rendering operations.
 * For example, we divide the screen into a status bar(VOLC_DISPLAY_OBJ_STATUS), 
 * main screen(VOLC_DISPLAY_OBJ_MAIN), and subtitle display bar(VOLC_DISPLAY_OBJ_SUBTITLE).
 * You may define your own divisions, as all these codes are open-source and modifiable.
 */
typedef enum {
    VOLC_DISPLAY_OBJ_STATUS = 0, // Status display object
    VOLC_DISPLAY_OBJ_MAIN,          
    VOLC_DISPLAY_OBJ_SUBTITLE,   // Subtitle display object
    // VOLC_DISPLAY_OBJ_EMOJI,   // Emoji display object
    VOLC_DISPLAY_OBJ_MAX,        // Maximum display object count
} volc_hal_display_obj_e;

typedef enum {
    VOLC_DISPLAY_TEXT = 0, // Text display type
    VOLC_DISPLAY_IMAGE,    // Image display type
    VOLC_DISPLAY_EMOJI     // Emoji display type
} volc_hal_display_type_e;

typedef struct volc_hal_display_config {
    int width;             // Width of the display
    int height;            // Height of the display
} volc_hal_display_config_t;

/**
 * @brief Create a Display instance
 * 
 * @param config Display configuration
 * @return volc_hal_display_t Display instance pointer
 */
volc_hal_display_t volc_hal_display_create(volc_hal_display_config_t* config);

/**
 * @brief set brightness 
 * 
 * @param display Display instance pointer
 * @param brightness the brightness value [0,100] 
 * @return volc_hal_display_t Display instance pointer
 */
int volc_hal_display_set_brightness(volc_hal_display_t display, int brightness);

/**
 * @brief Destroy a Display instance
 * 
 * @param display Display instance pointer
 */
void volc_hal_display_destroy(volc_hal_display_t display);

/**
 * @brief Set the display emotion
 * 
 * @param display Display instance pointer
 * @param type Display type (VOLC_DISPLAY_TEXT, VOLC_DISPLAY_IMAGE, or VOLC_DISPLAY_EMOJI)
 * @param content Content to be displayed (string for text, image pointer for image, emoji string for emoji)
 * @return int 0 if success, otherwise error code
 */
int volc_hal_display_set_content(volc_hal_display_t display, volc_hal_display_obj_e obj, volc_hal_display_type_e type, const void* content);

#ifdef __cplusplus
}
#endif

#endif