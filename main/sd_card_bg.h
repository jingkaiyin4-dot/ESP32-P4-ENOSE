/*
 * SD Card Background Image Loader
 * Loads a JPEG image from SD card and displays it as LVGL background
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief Initialize SD card and mount filesystem (if not already mounted)
 */
esp_err_t sd_card_bg_init(void);

/**
 * @brief Load a JPEG image into a specific slot
 * @param file_path  Full path to JPEG file
 * @param slot       Slot index (0 = startup, 1 = main)
 * @return lv_image_dsc_t*  Pointer to slot's image descriptor, or NULL on error
 */
const lv_image_dsc_t *sd_card_bg_load_jpeg_slot(const char *file_path, int slot);

/**
 * @brief Load a JPEG from SD card (uses slot 0 for backward compatibility)
 * @param file_path  Full path to JPEG file
 * @return lv_image_dsc_t*  Pointer to slot 0's descriptor, or NULL on error
 */
const lv_image_dsc_t *sd_card_bg_load_jpeg(const char *file_path);

/**
 * @brief Set an image as background of an LVGL object (screen or panel)
 * @param parent     Target LVGL object
 * @param img_dsc    Image descriptor from sd_card_bg_load_jpeg()
 * @return lv_obj_t* The created image object, or NULL on error
 */
lv_obj_t *sd_card_bg_set(lv_obj_t *parent, const lv_image_dsc_t *img_dsc);

/**
 * @brief Convenience: load JPEG and set as background in one call
 * @param parent     Target LVGL object
 * @param file_path  Full path to JPEG file
 * @return lv_obj_t* The created image object, or NULL on error
 */
lv_obj_t *sd_card_bg_set_from_file(lv_obj_t *parent, const char *file_path);

/**
 * @brief Check if SD card is ready
 */
bool sd_card_bg_is_ready(void);

#ifdef __cplusplus
}
#endif
