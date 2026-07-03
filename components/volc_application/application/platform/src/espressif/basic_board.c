/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2025 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"

#include "esp_board_manager.h"
#include "dev_audio_codec.h"
#include "dev_display_lcd.h"

#include "basic_board.h"

static const char *TAG = "BASIC_BOARD";

typedef struct {
    const char  *name;
    const char  *mic_layout;
    uint32_t     sample_rate;
    uint8_t      sample_bits;
    uint8_t      channels;
} board_config_t;

static const board_config_t supported_boards[] = {
    {"esp32_s3_korvo2_v3", "RMNM", 16000, 32, 2},
    {"esp_vocat_board_v1_2", "RMNM", 16000, 32, 2},
    {"esp_box_3", "RMNM", 16000, 32, 2},
    {"ESP32_S3_Korvo_2L", "MR", 16000, 16, 2},
};

static const size_t supported_boards_count = sizeof(supported_boards) / sizeof(supported_boards[0]);

#define DEFAULT_PLAY_VOLUME      30
#define DEFAULT_REC_GAIN         32.0
#define DEFAULT_REC_CHANNEL_GAIN 32.0
#define REC_CHANNEL_MASK         BIT(2)

static inline bool is_board_name(const char *board_name, const char *target_name)
{
    ESP_LOGI(TAG, "board_name: %s, target_name: %s", board_name, target_name);
    return strcmp(board_name, target_name) == 0;
}

static     dev_audio_codec_handles_t *play_dev_handles2 = NULL;
static dev_audio_codec_handles_t *rec_dev_handles2 = NULL;

static esp_err_t setup_board_devices(basic_board_periph_t *periph)
{
    esp_err_t ret = ESP_OK;
    dev_audio_codec_handles_t *play_dev_handles = NULL;
    dev_audio_codec_handles_t *rec_dev_handles = NULL;
    void *lcd_handle = NULL;

    esp_board_manager_init();
    // esp_board_manager_print();

    ret = esp_board_device_init_all();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize board devices: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_board_device_show("fs_sdcard");

    ret = esp_board_manager_get_device_handle("audio_dac", (void **)&play_dev_handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get audio_dac handle: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_board_manager_get_device_handle("audio_adc", (void **)&rec_dev_handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get audio_adc handle: %s", esp_err_to_name(ret));
        return ret;
    }

    periph->play_dev = play_dev_handles->codec_dev;
    periph->rec_dev = rec_dev_handles->codec_dev;
    play_dev_handles2 = play_dev_handles;
    rec_dev_handles2 = rec_dev_handles;
    ret = esp_board_manager_get_device_handle("display_lcd", &lcd_handle);
    if (ret == ESP_OK && lcd_handle) {
        dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
        periph->lcd_pannel = lcd_handles->panel_handle;
        ESP_LOGI(TAG, "LCD panel handle: %p", periph->lcd_pannel);
    } else {
        ESP_LOGW(TAG, "Failed to get LCD device handle: %s", esp_err_to_name(ret));
    }

    if (periph->play_dev) {
        ret = esp_codec_dev_set_out_vol(periph->play_dev, DEFAULT_PLAY_VOLUME);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set playback volume: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Playback device configured with volume: %d", DEFAULT_PLAY_VOLUME);
    }

    if (periph->rec_dev) {
        ret = esp_codec_dev_set_in_gain(periph->rec_dev, DEFAULT_REC_GAIN);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set recording gain: %s", esp_err_to_name(ret));
            return ret;
        }

        // ret = esp_codec_dev_set_in_channel_gain(periph->rec_dev, REC_CHANNEL_MASK, DEFAULT_REC_CHANNEL_GAIN);
        // if (ret != ESP_OK) {
        //     ESP_LOGE(TAG, "Failed to set recording channel gain: %s", esp_err_to_name(ret));
        //     // return ret;
        // }
        ESP_LOGI(TAG, "Recording device configured with gain: %.1f", DEFAULT_REC_GAIN);
    }


    
    return ESP_OK;
}

static esp_err_t configure_board_settings(basic_board_periph_t *periph)
{
    const board_config_t *config = NULL;

    for (size_t i = 0; i < supported_boards_count; i++) {
        ESP_LOGI(TAG, "g_esp_board_info.name: %s, supported_boards[i].name: %s", g_esp_board_info.name, supported_boards[i].name);
        if (is_board_name(g_esp_board_info.name, supported_boards[i].name)) {
            config = &supported_boards[i];
            break;
        }
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "Unsupported board: %s", g_esp_board_info.name);
        ESP_LOGE(TAG, "Supported boards:");
        for (size_t i = 0; i < supported_boards_count; i++) {
            ESP_LOGE(TAG, "  - %s", supported_boards[i].name);
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    strcpy(periph->mic_layout, config->mic_layout);
    periph->sample_rate = config->sample_rate;
    periph->sample_bits = config->sample_bits;
    periph->channels = config->channels;

    ESP_LOGI(TAG, "Board: %s", g_esp_board_info.name);
    ESP_LOGI(TAG, "Mic layout: %s", periph->mic_layout);
    ESP_LOGI(TAG, "Sample rate: %d Hz", periph->sample_rate);
    ESP_LOGI(TAG, "Sample bits: %d", periph->sample_bits);
    ESP_LOGI(TAG, "Channels: %d", periph->channels);

    return ESP_OK;
}

esp_err_t basic_board_init(basic_board_periph_t *periph)
{
    esp_err_t ret = ESP_OK;

    if (periph == NULL) {
        ESP_LOGE(TAG, "Invalid parameter: periph is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    memset(periph, 0, sizeof(basic_board_periph_t));

    ret = setup_board_devices(periph);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup board devices");
        return ret;
    }
    ret = configure_board_settings(periph);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure board settings");
        return ret;
    }
    ESP_LOGI(TAG, "Basic board initialization completed successfully");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = periph->sample_rate,
        .channel = periph->channels,
        .bits_per_sample = periph->sample_bits,
    };

    ESP_LOGI(TAG, "sample_rate: %d, channel: %d, bits: %d", fs.sample_rate, fs.channel, fs.bits_per_sample);

    if (periph->play_dev) {
        ret = esp_codec_dev_open(periph->play_dev, &fs);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open audio dac <%p>", periph->play_dev);
            return ret;
        }
    }
    if (periph->rec_dev) {
        ret = esp_codec_dev_open(periph->rec_dev, &fs);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open audio_adc <%p>", periph->rec_dev);
            return ret;
        }
    }

    return ESP_OK;
}