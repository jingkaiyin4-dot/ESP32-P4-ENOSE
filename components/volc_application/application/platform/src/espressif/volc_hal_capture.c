// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>

#include "volc_hal.h"
#include "volc_hal_capture.h"
#include "volc_osal.h"

#include "audio_processor.h"
#include "av_processor_type.h"
#include "basic_board.h"

#include "esp_codec_dev.h"
#include "util/volc_log.h"

typedef enum {
    WAKEUP = 0,
    CAPTURE,
    INITED,
} audio_pipeline_state_e;

typedef struct {
    av_processor_encoder_config_t recorder_cfg;
    audio_pipeline_state_e   state;
} volc_hal_audio_capture_config_t;

typedef struct {
    //TODO: add video capture config
    void* video_capture_config;
} volc_hal_video_capture_config_t;

typedef struct {
    volc_media_type_e media_type;
    union {
        volc_hal_audio_capture_config_t audio_capture_config;
        volc_hal_video_capture_config_t video_capture_config;
    };
    volc_hal_capture_data_cb_t data_cb; // Data callback function
    volc_hal_audio_wakeup_cb_t audio_wakeup_cb; // audio wakeup cb
    void*                      user_data;               // User data pointer
    volc_osal_tid_t            capture_thread;
    volatile bool              is_started;
} volc_hal_capture_impl_t;

extern basic_board_periph_t global_periph;

static int __volc_capture_get_default_read_size(volc_hal_capture_t capture)
{
    // 60ms
#if (CONFIG_VOLC_AUDIO_G711A)
    return (160*3);
#else
    return 960;
#endif
}

static void __volc_audio_capture_task(void *arg)
{
    volc_hal_capture_impl_t *impl = (volc_hal_capture_impl_t *)arg;
    LOGI("capture task started...");
    int read_size = __volc_capture_get_default_read_size(impl);
    uint8_t *read_buf = volc_osal_malloc(read_size);
//     mem_assert(read_buf);

    while(impl->is_started) {
        int ret = audio_recorder_read_data(read_buf, read_size);

        if (ret > 0 && impl->data_cb && impl->audio_capture_config.state == CAPTURE) {
        #ifdef CONFIG_VOLC_AUDIO_G711A
            volc_hal_frame_info_t frame_info = {
                .data_type = VOLC_AUDIO_DATA_TYPE_G711A,
                .user_data = impl->user_data,
            };
            impl->data_cb(impl, (const void *)read_buf, ret, &frame_info);
        #else
            volc_hal_frame_info_t frame_info = {
                .data_type = VOLC_AUDIO_DATA_TYPE_PCM,
                .user_data = impl->user_data,
            };
            impl->data_cb(impl, (const void *)read_buf, ret, &frame_info);
        #endif
        }
    }
    // audio_recorder_pause();
    if (impl->capture_thread) {
        volc_osal_thread_destroy(impl->capture_thread);
        impl->capture_thread = NULL;
    }
    volc_osal_free(read_buf);
    volc_osal_thread_exit(NULL);
}

volc_hal_capture_t volc_hal_capture_create(volc_hal_capture_config_t *config)
{
    volc_hal_context_t *g_hal_context = volc_get_global_hal_context();
    if (g_hal_context == NULL)
    {
        return NULL;
    }
    volc_hal_capture_impl_t *capture = (volc_hal_capture_impl_t *)volc_osal_calloc(1, sizeof(volc_hal_capture_impl_t));
    // config
    capture->is_started = false;
    capture->media_type = config->media_type;
    capture->data_cb = config->data_cb;
    capture->audio_wakeup_cb = config->audio_wakeup_cb;
    capture->user_data = config->user_data;
    esp_codec_dev_set_in_gain(global_periph.rec_dev, 52.0);
    switch (capture->media_type)
    {
    case VOLC_MEDIA_TYPE_AUDIO:
#if (CONFIG_VOLC_AUDIO_G711A)
        capture->audio_capture_config.recorder_cfg.format = AV_PROCESSOR_FORMAT_ID_G711A;
#else
        capture->audio_capture_config.recorder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
#endif
        capture->audio_capture_config.recorder_cfg.params.g711.audio_info.sample_rate = 16000;
        capture->audio_capture_config.recorder_cfg.params.g711.audio_info.sample_bits = 16;
        capture->audio_capture_config.recorder_cfg.params.g711.audio_info.channels = 1;
        capture->audio_capture_config.recorder_cfg.params.g711.audio_info.frame_duration = 60;
        capture->audio_capture_config.state = INITED;
        g_hal_context->capture_handle[VOLC_HAL_CAPTURE_AUDIO] = (volc_hal_capture_t)capture;
        break;
    case VOLC_MEDIA_TYPE_VIDEO:
        // TODO: Add video capture support
        LOGE("Video capture is not supported yet");
        break;
    default:
        break;
    }
    return (volc_hal_capture_t)capture;
}

static void __start_audio_capture(volc_hal_capture_impl_t *capture)
{
//     audio_pipeline_unlink(impl->audio_capture_config.audio_pipeline);
    av_processor_afe_config_t afe_config = DEFAULT_AV_PROCESSOR_AFE_CONFIG();
    afe_config.ai_mode_wakeup = false;
    audio_recorder_config_t recorder_config = DEFAULT_AUDIO_RECORDER_CONFIG();
    memcpy((void *)&recorder_config.encoder_cfg, &capture->audio_capture_config.recorder_cfg, sizeof(av_processor_encoder_config_t));
    memcpy(&recorder_config.afe_config, &afe_config, sizeof(av_processor_afe_config_t));
    recorder_config.recorder_event_cb = capture->audio_wakeup_cb;
    recorder_config.recorder_ctx = NULL;
    audio_recorder_open(&recorder_config);

    volc_osal_thread_param_t param = {0};
    snprintf(param.name, sizeof(param.name), "%s", "volc_capture_task");
    param.stack_size = 4 * 1024;
    param.priority = 5;
    param.stack_in_ext = 1;
    capture->is_started = true;
    capture->audio_capture_config.state = CAPTURE;
    esp_err_t ret = volc_osal_thread_create(&capture->capture_thread, &param, __volc_audio_capture_task, capture);
    
    LOGI("capture task start... mode: CAPTURE");
    return;
}

static void __start_audio_wake(volc_hal_capture_impl_t *capture)
{
 av_processor_afe_config_t afe_config = DEFAULT_AV_PROCESSOR_AFE_CONFIG();
    afe_config.ai_mode_wakeup = true;
    audio_recorder_config_t recorder_config = DEFAULT_AUDIO_RECORDER_CONFIG();
    memcpy((void *)&recorder_config.encoder_cfg, &capture->audio_capture_config.recorder_cfg, sizeof(av_processor_encoder_config_t));
    memcpy(&recorder_config.afe_config, &afe_config, sizeof(av_processor_afe_config_t));
    recorder_config.recorder_event_cb = capture->audio_wakeup_cb;
    recorder_config.recorder_ctx = NULL;
    audio_recorder_open(&recorder_config);
    
    volc_osal_thread_param_t param = {0};
    snprintf(param.name, sizeof(param.name), "%s", "volc_capture_task");
    param.stack_size = 4 * 1024;
    param.priority = 5;
    param.stack_in_ext = 1;
    capture->is_started = true;
    capture->audio_capture_config.state = WAKEUP;
    esp_err_t ret = volc_osal_thread_create(&capture->capture_thread, &param, __volc_audio_capture_task, capture);

    LOGI("capture task start... mode: WAKEUP");
    return;
}

int volc_hal_capture_start(volc_hal_capture_t capture,volc_hal_audio_capture_mode_e mode)
{
    if(capture == NULL){
        return -1;
    }
    volc_hal_capture_impl_t *impl = (volc_hal_capture_impl_t *)capture;
    if (impl->media_type == VOLC_MEDIA_TYPE_AUDIO && impl->audio_capture_config.state == INITED) {
        switch (mode)
        {
        case VOLC_AUDIO_MODE_WAKEUP:
            __start_audio_wake(impl);
            break;
        case VOLC_AUDIO_MODE_CAPTURE:
            __start_audio_capture(impl);
            break;
        default:
            break;
        }
    }
    return 0;
}

int volc_hal_capture_stop(volc_hal_capture_t capture)
{
    volc_hal_capture_impl_t *impl = (volc_hal_capture_impl_t *)capture;

    if (!impl || !impl->is_started) {
        return 0;
    }
    // stop thread
    impl->is_started = false;
    switch (impl->media_type) {
        case VOLC_MEDIA_TYPE_AUDIO:
            // audio_recorder_pause();
            // audio_recorder_resume();
            audio_recorder_close();
            impl->audio_capture_config.state = INITED;
            break;
        case VOLC_MEDIA_TYPE_VIDEO:
            // TODO: Add video capture support
            LOGE("Video capture is not supported yet");
            break;
        default:
            break;
    }
    return 0;
}

void volc_hal_capture_destroy(volc_hal_capture_t capture)
{
    volc_hal_context_t* g_hal_context = volc_get_global_hal_context();
    if(g_hal_context == NULL){
        return;
    }
    volc_hal_capture_impl_t *impl = (volc_hal_capture_impl_t *)capture;
    if (!impl) {
        return;
    }

    volc_hal_capture_stop(capture);
    switch (impl->media_type) {
        case VOLC_MEDIA_TYPE_AUDIO:
            g_hal_context->capture_handle[VOLC_HAL_CAPTURE_AUDIO] = NULL;
            break;
        case VOLC_MEDIA_TYPE_VIDEO:
            // TODO: Add video capture support
            LOGE("Video capture is not supported yet");
            break;
        default:
            break;
    }
    volc_osal_free(impl);
}

int volc_hal_capture_update_config(volc_hal_capture_t capture, volc_hal_capture_config_t* config) {
    volc_hal_capture_impl_t *impl = (volc_hal_capture_impl_t *)capture;
    if (!impl || !config) {
        LOGE("capture or config is NULL");
        return -1;
    }

    if (impl->media_type != config->media_type) {
        LOGE("media_type not match");
        return -1;
    }

    if (impl->user_data != config->user_data) {
        LOGE("user_data not match");
        return -1;
    }

    if (impl->is_started) {
        LOGE("capture is started, can not update config");
        return -1;
    }
    if(config->data_cb){
        impl->data_cb = config->data_cb;
    }
    if(config->audio_wakeup_cb){
        impl->audio_wakeup_cb = config->audio_wakeup_cb;
    }
    return 0;
}
