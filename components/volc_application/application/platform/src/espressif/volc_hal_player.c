// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>

#include "volc_hal.h"
#include "volc_hal_player.h"
#include "volc_osal.h"
#include "basic_board.h"

#include "audio_processor.h"
#include "esp_codec_dev.h"
#include "util/volc_log.h"

#define DEFAULT_AUDIO_VOL 70
typedef struct {
    av_processor_decoder_config_t feeder_cfg;
    int volume;
} audio_player_config_t;

typedef struct {
    //TODO: add video player config
    void* video_player_config;
} video_player_config_t;

typedef struct {
    volc_media_type_e media_type;   // Media type
    union {
        audio_player_config_t audio_player_config;
        video_player_config_t video_player_config;
    };
    volatile bool              is_started;
} volc_player_impl_t;

extern basic_board_periph_t global_periph;

volc_hal_player_t volc_hal_player_create(volc_hal_player_config_t* config)
{
    volc_hal_context_t* g_hal_context = volc_get_global_hal_context();
    if(g_hal_context == NULL){
        return NULL;
    }
    volc_player_impl_t* impl = volc_osal_calloc(1, sizeof(volc_player_impl_t));
    // mem_assert(impl);
    impl->media_type = config->media_type;
    switch (impl->media_type) {
        case VOLC_MEDIA_TYPE_AUDIO:
            if(global_periph.play_dev){
                esp_codec_dev_set_out_vol(global_periph.play_dev, DEFAULT_AUDIO_VOL);
            }
            impl->audio_player_config.volume = DEFAULT_AUDIO_VOL;
            impl->audio_player_config.feeder_cfg.params.g711.audio_info.sample_rate = 16000;
            impl->audio_player_config.feeder_cfg.params.g711.audio_info.sample_bits = 16;
            impl->audio_player_config.feeder_cfg.params.g711.audio_info.channels = 1;
            impl->audio_player_config.feeder_cfg.params.g711.audio_info.frame_duration = 20;
#if (CONFIG_VOLC_AUDIO_G711A)
            impl->audio_player_config.feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_G711A;
#else
            impl->audio_player_config.feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
#endif
            audio_feeder_config_t feeder_config = DEFAULT_AUDIO_FEEDER_CONFIG();
            memcpy((void *)&feeder_config.decoder_cfg, &impl->audio_player_config.feeder_cfg, sizeof(av_processor_decoder_config_t));
            audio_feeder_open(&feeder_config);
            // audio_processor_mixer_open();
            impl->is_started = false;
            g_hal_context->player_handle[VOLC_HAL_PLAYER_AUDIO] = impl;
            break;
        case VOLC_MEDIA_TYPE_VIDEO:
            // TODO: Add video player support
            break;
        default:
            break;
    }
    return (volc_hal_player_t)impl;
}

int volc_hal_set_audio_player_volume(volc_hal_player_t player, int volume){
    volc_player_impl_t* impl = (volc_player_impl_t*)player;
    if (impl == NULL) {
        return -1;
    }
    if(impl->media_type == VOLC_MEDIA_TYPE_AUDIO){
        impl->audio_player_config.volume = volume;
        return esp_codec_dev_set_out_vol(global_periph.play_dev, volume);
    } else if(impl->media_type == VOLC_MEDIA_TYPE_VIDEO){
        LOGE("media type %d not supported set volume", impl->media_type);
        return -1;
    }
}

int volc_hal_get_audio_player_volume(volc_hal_player_t player){
    volc_player_impl_t* impl = (volc_player_impl_t*)player;
    if (impl == NULL) {
        return -1;
    }
    return impl->audio_player_config.volume;
}

void volc_hal_player_destroy(volc_hal_player_t player)
{
    volc_hal_context_t* g_hal_context = volc_get_global_hal_context();
    if(g_hal_context == NULL){
        return;
    }
    volc_player_impl_t* impl = (volc_player_impl_t*)player;
    if (impl == NULL) {
        return;
    }
    volc_hal_player_stop(impl);
    switch (impl->media_type) {
        case VOLC_MEDIA_TYPE_AUDIO:
            audio_feeder_close();

            g_hal_context->player_handle[VOLC_HAL_PLAYER_AUDIO] = NULL;
            break;
        case VOLC_MEDIA_TYPE_VIDEO:
            // TODO: Add video player support
            break;
        default:
            break;
    }
    volc_osal_free(impl);
}

int volc_hal_player_start(volc_hal_player_t player)
{
    volc_player_impl_t* impl = (volc_player_impl_t*)player;
    if (impl == NULL) {
        LOGE("volc_hal_player_start: impl is NULL");
        return -1;
    }
    if(impl->is_started) return 0;
    switch (impl->media_type) {
        case VOLC_MEDIA_TYPE_AUDIO:
            impl->is_started = true;
            return audio_feeder_run();
        case VOLC_MEDIA_TYPE_VIDEO:
            // TODO: Add video player support
            break;
        default:
            break;
    }
    LOGE("media type %d not supported", impl->media_type);
    return -1;
}

int volc_hal_player_stop(volc_hal_player_t player)
{
    volc_player_impl_t* impl = (volc_player_impl_t*)player;
    if (impl == NULL) {
        LOGE("volc_hal_player_stop: impl is NULL");
        return -1;
    }
    if(impl->is_started == false) return 0;
    switch (impl->media_type) {
        case VOLC_MEDIA_TYPE_AUDIO:
            impl->is_started = false;
            return audio_feeder_stop();
            break;
        case VOLC_MEDIA_TYPE_VIDEO:
            // TODO: Add video player support
            break;
        default:
            break;
    }
    LOGE("media type %d not supported", impl->media_type);
    return -1;
}

int volc_hal_player_play_data(volc_hal_player_t player, const void* data, int len)
{
    if (player == NULL || data == NULL || len <= 0) {
        LOGE("player play data failed, player is %p, data is %p, len is %d", player, data, len);
        return -1;
    }
    volc_player_impl_t* impl = (volc_player_impl_t*)player;
    switch (impl->media_type) {
        case VOLC_MEDIA_TYPE_AUDIO:
            return audio_feeder_feed_data((uint8_t*)data, len);
            break;
        case VOLC_MEDIA_TYPE_VIDEO:
            // TODO: Add video player support
            break;
        default:
            break;
    }
    LOGE("media type %d not supported", impl->media_type);
    return -1;
}
