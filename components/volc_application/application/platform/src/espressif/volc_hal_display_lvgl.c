// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#include "volc_hal.h"
#include "volc_hal_display.h"
#include "volc_lvgl_source.h"
#include "lvgl.h"
#include "bsp/echoear.h"
#include <stddef.h>
#include "volc_osal.h"
#include  <string.h>

#define LVGL_TASK_PRIORITY       1
#define LVGL_TASK_CORE_ID        1
#define LVGL_TASK_STACK_SIZE     8 * 1024
#define LVGL_TASK_MAX_SLEEP_MS   500
#define LVGL_TASK_TIMER_PERIOD_MS 50
#define LVGL_TIMER_INTERVAL      500

typedef struct volc_hal_display_impl {
    lv_obj_t* screen; 
    lv_obj_t* display_obj[VOLC_DISPLAY_OBJ_MAX];
    //  use for display subtitle
    lv_timer_t* text_timer;
    char subtitle_texts[64];
    char status_texts[64];
    volatile bool   is_init;
} volc_hal_display_impl_t;

static volc_hal_display_impl_t* global_display_impl = NULL;
volc_hal_display_t  global_display = NULL;

static void __subtitle_update_cb(lv_timer_t* timer)
{
    volc_hal_context_t* g_hal_context = volc_get_global_hal_context();
    if(g_hal_context == NULL){
        return;
    }
    volc_hal_display_impl_t* global_display_impl = (volc_hal_display_impl_t*)(g_hal_context->display_handle);
    if(global_display_impl && global_display_impl->is_init){
        lv_label_set_text(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_SUBTITLE], global_display_impl->subtitle_texts);
        lv_label_set_text(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_STATUS], global_display_impl->status_texts);
        lv_obj_remove_flag(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_SUBTITLE], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_STATUS], LV_OBJ_FLAG_HIDDEN);
    }
}

static void __status_obj_init(volc_hal_display_impl_t* global_display_impl)
{
    if(global_display_impl && global_display_impl->display_obj[VOLC_DISPLAY_OBJ_STATUS] == NULL){
        global_display_impl->display_obj[VOLC_DISPLAY_OBJ_STATUS] = lv_label_create(global_display_impl->screen);
        lv_obj_set_style_text_font(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_STATUS], &echoear_font_16, 0);
        lv_obj_align(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_STATUS], LV_ALIGN_TOP_MID, 0, 50);
        lv_obj_set_height(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_STATUS], 30);  // 设置固定高度度
    }
}

static void __subtitle_obj_init(volc_hal_display_impl_t* global_display_impl)
{
    if(global_display_impl && global_display_impl->display_obj[VOLC_DISPLAY_OBJ_SUBTITLE] == NULL){
        global_display_impl->display_obj[VOLC_DISPLAY_OBJ_SUBTITLE] = lv_label_create(global_display_impl->screen);
        lv_obj_set_style_text_font(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_SUBTITLE], &echoear_font_16, 0);
        lv_obj_align(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_SUBTITLE], LV_ALIGN_BOTTOM_MID, 0, -50);
        lv_obj_set_height(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_SUBTITLE], 30);  // 设置固定高度度
    }
    if(global_display_impl && global_display_impl->text_timer == NULL){
        global_display_impl->text_timer = lv_timer_create(__subtitle_update_cb, LVGL_TIMER_INTERVAL, NULL);
    }
}

static void __main_obj_init(volc_hal_display_impl_t* global_display_impl)
{
    if(global_display_impl && global_display_impl->display_obj[VOLC_DISPLAY_OBJ_MAIN] == NULL){
        global_display_impl->display_obj[VOLC_DISPLAY_OBJ_MAIN] = lv_img_create(global_display_impl->screen);
        //  size需要调整
        lv_obj_set_size(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_MAIN], 250, 250);
        lv_obj_align(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_MAIN], LV_ALIGN_CENTER, 0, 0);
    }
}

volc_hal_display_t volc_hal_display_create(volc_hal_display_config_t* config)
{

    volc_hal_context_t* g_hal_context = volc_get_global_hal_context();
    if(g_hal_context == NULL){
        return NULL;
    }

    volc_hal_display_impl_t* global_display_impl = (volc_hal_display_impl_t*)volc_osal_calloc(1,sizeof(volc_hal_display_impl_t));
    // init display
    // bsp_display_cfg_t cfg = {
    //     .lvgl_port_cfg = {
    //         .task_priority = LVGL_TASK_PRIORITY,
    //         .task_stack = LVGL_TASK_STACK_SIZE,
    //         .task_affinity = LVGL_TASK_CORE_ID,
    //         .task_max_sleep_ms = LVGL_TASK_MAX_SLEEP_MS,
    //         .task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    //         .timer_period_ms = LVGL_TASK_TIMER_PERIOD_MS,
    //     },
    //     .buffer_size = BSP_LCD_H_RES * 50,
    //     .double_buffer = true,
    //     .flags = {
    //         .buff_spiram = false,
    //     // .default_dummy_draw = default_dummy_draw, // Avoid white screen during initialization
    //     },
    // };
    // lv_disp_t *disp = bsp_display_start_with_config(&cfg);
    // // bsp_display_brightness_init();
    // bsp_display_backlight_on();
    global_display_impl->screen = lv_scr_act();
    // printf("global_display_impl->screen %p \n",global_display_impl->screen);
    __subtitle_obj_init(global_display_impl);
    __status_obj_init(global_display_impl);
    __main_obj_init(global_display_impl);
    global_display_impl->is_init = true;

    g_hal_context->display_handle = (volc_hal_display_t)global_display_impl;
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
    return bsp_display_brightness_set(brightness);
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
        lv_timer_del(global_display_impl->text_timer);
        lv_obj_del(global_display_impl->screen);
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
        memset(global_display_impl->status_texts, 0, 64);
        int count = strlen(content) >= 63 ? 63 : strlen(content);
        memcpy(global_display_impl->status_texts,content,count);
    }
    if(obj == VOLC_DISPLAY_OBJ_SUBTITLE && type == VOLC_DISPLAY_TEXT){
        memset(global_display_impl->subtitle_texts, 0, 64);
        int count = strlen(content) >= 63 ? 63 : strlen(content);
        memcpy(global_display_impl->subtitle_texts,content,count);
    }
    if(obj == VOLC_DISPLAY_OBJ_MAIN && type == VOLC_DISPLAY_IMAGE){
        lv_image_dsc_t* img_app_pos = (lv_image_dsc_t*)content;
        // lv_obj_set_size(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_MAIN], 112, 112);
        lv_img_set_src(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_MAIN], img_app_pos);
        lv_obj_remove_flag(global_display_impl->display_obj[VOLC_DISPLAY_OBJ_MAIN] ,LV_OBJ_FLAG_HIDDEN);
    }
    return 0;
}
