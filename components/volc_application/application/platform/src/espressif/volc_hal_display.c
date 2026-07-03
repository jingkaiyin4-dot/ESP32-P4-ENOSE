// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#include "volc_hal.h"
#include "volc_hal_display.h"
#include "volc_lvgl_source.h"
#include "hw_init.h"
#include "esp_lcd_panel_ops.h"
#include "esp_mmap_assets.h"
#include "esp_board_manager.h"
#include "dev_ledc_ctrl.h"
#include "gfx.h"
#include "lvgl.h"
#include <stddef.h>
#include "volc_osal.h"
#include  <string.h>
#include "basic_board.h"

#include "mmap_generate_eaf.h"

typedef struct volc_hal_display_impl {
    volatile bool   is_init;
    /* Global state */
    esp_lcd_panel_handle_t s_panel;
    mmap_assets_handle_t s_assets;
    gfx_handle_t s_gfx;
    gfx_obj_t *s_anim;
    gfx_obj_t *s_label;
    esp_lcd_panel_io_handle_t display_io_handle;
    int s_current_emote;
    volc_osal_tid_t            display_thread;
} volc_hal_display_impl_t;

static volc_hal_display_impl_t* global_display_impl = NULL;
extern basic_board_periph_t global_periph;

static esp_lv_adapter_rotation_t __get_configured_rotation(void)
{
#if CONFIG_EXAMPLE_DISPLAY_ROTATION_0
    return ESP_LV_ADAPTER_ROTATE_0;
#elif CONFIG_EXAMPLE_DISPLAY_ROTATION_90
    return ESP_LV_ADAPTER_ROTATE_90;
#elif CONFIG_EXAMPLE_DISPLAY_ROTATION_180
    return ESP_LV_ADAPTER_ROTATE_180;
#elif CONFIG_EXAMPLE_DISPLAY_ROTATION_270
    return ESP_LV_ADAPTER_ROTATE_270;
#else
    return ESP_LV_ADAPTER_ROTATE_0;
#endif
}

/* ========== Asset Management ========== */

static esp_err_t __mount_assets(void)
{
    mmap_assets_config_t cfg = {
        .partition_label = "eaf",
        .max_files = MMAP_EAF_FILES,
        .checksum = MMAP_EAF_CHECKSUM,
        .flags = {
            .mmap_enable = 1,
        },
    };

    return mmap_assets_new(&cfg, &global_display_impl->s_assets);
}

/* ========== GFX Emote Engine ========== */

static void __gfx_flush_cb(gfx_handle_t handle, int x1, int y1, int x2, int y2, const void *data)
{
    if (global_display_impl->s_panel) {
        esp_lcd_panel_draw_bitmap(global_display_impl->s_panel, x1, y1, x2, y2, data);
    }
    gfx_emote_flush_ready(handle, true);
}

static esp_err_t __init_gfx_engine(void)
{
    gfx_core_config_t cfg = {
        .flush_cb = __gfx_flush_cb,
        .user_data = global_display_impl->s_panel,
        .flags = {
            .swap = 1,
            .double_buffer = 1,
            .buff_dma = 0,
            .buff_spiram = 1,
        },
        .h_res = HW_LCD_H_RES,
        .v_res = HW_LCD_V_RES,
        .fps = 30,
        .task = {
            .task_priority = 4,
            .task_stack = 7168,
            .task_affinity = -1,
            .task_stack_caps = MALLOC_CAP_SPIRAM,
        },
        .buffers = {
            .buf_pixels = HW_LCD_H_RES * HW_LCD_V_RES,
        },
    };

    global_display_impl->s_gfx = gfx_emote_init(&cfg);
    return global_display_impl->s_gfx ? ESP_OK : ESP_FAIL;
}


static esp_err_t __create_font_overlay(char* text)
{
    gfx_emote_lock(global_display_impl->s_gfx);
    if(global_display_impl->s_label == NULL){
        global_display_impl->s_label = gfx_label_create(global_display_impl->s_gfx);
    }
    if (!global_display_impl->s_label) {
        gfx_emote_unlock(global_display_impl->s_gfx);
        return ESP_ERR_NO_MEM;
    }

    /* Configure label: 200px width, positioned at top center */
    gfx_obj_set_size(global_display_impl->s_label, 200, 40);
    gfx_label_set_font(global_display_impl->s_label, (gfx_font_t)&echoear_font_16);
    gfx_label_set_text(global_display_impl->s_label, text);
    gfx_label_set_color(global_display_impl->s_label, GFX_COLOR_HEX(0xFFFFFF));
    gfx_label_set_text_align(global_display_impl->s_label, GFX_TEXT_ALIGN_CENTER);
    gfx_obj_align(global_display_impl->s_label, GFX_ALIGN_TOP_MID, 0, 30);

    gfx_emote_unlock(global_display_impl->s_gfx);
    return ESP_OK;
}

static esp_err_t __create_animation(void)
{
    /* Get EAF data from mmap */
    const uint8_t *eaf_data = mmap_assets_get_mem(global_display_impl->s_assets, global_display_impl->s_current_emote);
    int eaf_size = mmap_assets_get_size(global_display_impl->s_assets, global_display_impl->s_current_emote);

    gfx_emote_lock(global_display_impl->s_gfx);

    /* Create animation object */
    global_display_impl->s_anim = gfx_anim_create(global_display_impl->s_gfx);
    if (!global_display_impl->s_anim) {
        gfx_emote_unlock(global_display_impl->s_gfx);
        return ESP_FAIL;
    }

    /* Set animation source */
    esp_err_t ret = gfx_anim_set_src(global_display_impl->s_anim, eaf_data, eaf_size);
    if (ret != ESP_OK) {
        gfx_obj_delete(global_display_impl->s_anim);
        global_display_impl->s_anim = NULL;
        gfx_emote_unlock(global_display_impl->s_gfx);
        return ret;
    }

    /* Get animation dimensions from asset metadata */
    int width = mmap_assets_get_width(global_display_impl->s_assets, global_display_impl->s_current_emote);
    int height = mmap_assets_get_height(global_display_impl->s_assets, global_display_impl->s_current_emote);
    if (width <= 0 || height <= 0) {
        width = HW_LCD_H_RES;
        height = HW_LCD_V_RES;
    }

    gfx_obj_set_size(global_display_impl->s_anim, width, height);
    gfx_anim_set_mirror(global_display_impl->s_anim, true, 50);  /* Enable mirror for dual-eye display */
    if(global_display_impl->s_current_emote == 0){
        gfx_obj_align(global_display_impl->s_anim, GFX_ALIGN_LEFT_MID, 35, 0);
    } else {
        gfx_obj_align(global_display_impl->s_anim, GFX_ALIGN_CENTER, 0, 0);
    }
    gfx_anim_set_segment(global_display_impl->s_anim, 0, UINT32_MAX, 30, true);  /* All frames, 30 FPS, loop */
    gfx_emote_unlock(global_display_impl->s_gfx);
    return ESP_OK;
}

static void __emote_task(void *arg)
{
    /* Start animation playback */
    gfx_emote_lock(global_display_impl->s_gfx);
    gfx_anim_start(global_display_impl->s_anim);
    gfx_emote_unlock(global_display_impl->s_gfx);

    TickType_t last_switch_time = xTaskGetTickCount();

    /* Keep task alive and switch emotes every 3 seconds */
    while (1) {
        TickType_t current_time = xTaskGetTickCount();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t __switch_animation(int emote_index)
{
    if (!global_display_impl->s_anim || !global_display_impl->s_gfx) {
        return ESP_FAIL;
    }

    /* Get new EAF data from mmap */
    const uint8_t *eaf_data = mmap_assets_get_mem(global_display_impl->s_assets, emote_index);
    int eaf_size = mmap_assets_get_size(global_display_impl->s_assets, emote_index);
    if(global_display_impl->s_current_emote == emote_index) {
        return ESP_OK;
    }
    gfx_emote_lock(global_display_impl->s_gfx);

    /* Stop current animation */
    gfx_anim_stop(global_display_impl->s_anim);

    /* Update animation source */
    esp_err_t ret = gfx_anim_set_src(global_display_impl->s_anim, eaf_data, eaf_size);
    if (ret != ESP_OK) {
        gfx_emote_unlock(global_display_impl->s_gfx);
        return ret;
    }

    /* Get animation dimensions from asset metadata */
    int width = mmap_assets_get_width(global_display_impl->s_assets, emote_index);
    int height = mmap_assets_get_height(global_display_impl->s_assets, emote_index);
    if (width <= 0 || height <= 0) {
        width = HW_LCD_H_RES;
        height = HW_LCD_V_RES;
    }

    gfx_obj_set_size(global_display_impl->s_anim, width, height);
    gfx_anim_set_mirror(global_display_impl->s_anim, true, 50);  /* Enable mirror for dual-eye display */
    if(emote_index == 0){
        gfx_obj_align(global_display_impl->s_anim, GFX_ALIGN_LEFT_MID, 35, 0);
    } else {
        gfx_obj_align(global_display_impl->s_anim, GFX_ALIGN_CENTER, 0, 0);
    }

    gfx_anim_set_segment(global_display_impl->s_anim, 0, UINT32_MAX, 30, true);  /* All frames, 30 FPS, loop */

    /* Restart animation */
    gfx_anim_start(global_display_impl->s_anim);

    gfx_emote_unlock(global_display_impl->s_gfx);

    global_display_impl->s_current_emote = emote_index;
    return ESP_OK;
}

volc_hal_display_t volc_hal_display_create(volc_hal_display_config_t* config)
{

    volc_hal_context_t* g_hal_context = volc_get_global_hal_context();
    if(g_hal_context == NULL){
        return NULL;
    }

    global_display_impl = (volc_hal_display_impl_t*)volc_osal_calloc(1,sizeof(volc_hal_display_impl_t));
    // init display
 
    esp_lv_adapter_rotation_t rotation = __get_configured_rotation();
    /* Determine tear avoid mode based on LCD interface */
#if CONFIG_EXAMPLE_LCD_INTERFACE_MIPI_DSI
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI;
#elif CONFIG_EXAMPLE_LCD_INTERFACE_RGB
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
#else
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT;
#endif
    // ESP_ERROR_CHECK(hw_lcd_init(&global_display_impl->s_panel, &global_display_impl->display_io_handle, tear_avoid_mode, rotation));
    global_display_impl->s_panel = global_periph.lcd_pannel;
    ESP_ERROR_CHECK(__mount_assets());
    __init_gfx_engine();
    global_display_impl->s_current_emote = MMAP_EAF_BOOT_360_360_EAF;
    __create_animation();
    __create_font_overlay("");
    volc_osal_thread_param_t param = {0};
    snprintf(param.name, sizeof(param.name), "%s", "volc_capture_task");
    param.stack_size = 8 * 1024;
    param.priority = 4;
    param.stack_in_ext = 1;
    esp_err_t ret = volc_osal_thread_create(&global_display_impl->display_thread, &param, __emote_task, NULL);

    global_display_impl->is_init = true;

    g_hal_context->display_handle = (volc_hal_display_t)global_display_impl;
    volc_hal_display_set_brightness(global_display_impl,80);
    return (volc_hal_display_t)global_display_impl;
}

int volc_hal_display_set_brightness(volc_hal_display_t display, int brightness)
{
    if(display == NULL) return -1;
    if (brightness > 100) {
        brightness = 100;
    }
    if (brightness < 0) {
        brightness = 0;
    }
    
    periph_ledc_handle_t *ledc_handle = NULL;

    esp_board_manager_get_device_handle("lcd_brightness", (void **)&ledc_handle);

    dev_ledc_ctrl_config_t *dev_ledc_cfg = NULL;
    esp_err_t config_ret = esp_board_manager_get_device_config("lcd_brightness", (void*)&dev_ledc_cfg);
    if (config_ret != ESP_OK) {
        return -1;
    }
    // ESP_LOGI(TAG, "dev_ledc_cfg.ledc_name: %s, name: %s, type: %s", dev_ledc_cfg->ledc_name, dev_ledc_cfg->name, dev_ledc_cfg->type);
    periph_ledc_config_t *ledc_config = NULL;
    esp_board_manager_get_periph_config(dev_ledc_cfg->ledc_name, (void**)&ledc_config);
    uint32_t duty = (brightness * ((1 << (uint32_t)ledc_config->duty_resolution) - 1)) / 100;
    // ESP_LOGI(TAG, "duty_cycle: %" PRIu32 ", speed_mode: %d, channel: %d, duty_resolution: %d", duty, ledc_handle->speed_mode, ledc_handle->channel, ledc_config->duty_resolution);
    // ESP_BOARD_RETURN_ON_ERROR(ledc_set_duty(ledc_handle->speed_mode, ledc_handle->channel, duty), TAG, "LEDC set duty failed");
    ledc_set_duty(ledc_handle->speed_mode, ledc_handle->channel, duty);
    // ESP_BOARD_RETURN_ON_ERROR(ledc_update_duty(ledc_handle->speed_mode, ledc_handle->channel), TAG, "LEDC update duty failed");
    ledc_update_duty(ledc_handle->speed_mode, ledc_handle->channel);
    return 0;
    // return bsp_display_brightness_set(brightness);
}

void volc_hal_display_destroy(volc_hal_display_t display)
{
    volc_hal_context_t* g_hal_context = volc_get_global_hal_context();
    if(g_hal_context == NULL){
        return;
    }
    volc_hal_display_impl_t* global_display_impl = (volc_hal_display_impl_t*)display;
    if(global_display_impl && global_display_impl->is_init){
        global_display_impl->is_init = false;
    }
    volc_osal_free(global_display_impl);
    global_display_impl = NULL;
    g_hal_context->display_handle = NULL;
    return;
}

int volc_hal_display_set_content(volc_hal_display_t display, volc_hal_display_obj_e obj, volc_hal_display_type_e type, const void* content)
{
    volc_hal_context_t* g_hal_context = volc_get_global_hal_context();
    if(g_hal_context == NULL || g_hal_context->display_handle == NULL){
        return -1;
    }
    volc_hal_display_impl_t* global_display_impl = (volc_hal_display_impl_t*)(g_hal_context->display_handle);

    if(obj == VOLC_DISPLAY_OBJ_STATUS && type == VOLC_DISPLAY_TEXT){
        __create_font_overlay(content);
    }
    if(obj == VOLC_DISPLAY_OBJ_SUBTITLE && type == VOLC_DISPLAY_TEXT){
        return -1;
    }
    if(obj == VOLC_DISPLAY_OBJ_MAIN && type == VOLC_DISPLAY_IMAGE){
       int index = *((int*)content);
       if(index < MMAP_EAF_FILES){
           __switch_animation(index);
       }
    }
    return 0;
}
