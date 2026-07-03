// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#include "volc_hal_file.h"
#include "volc_hal.h"
#include "volc_osal.h"
#include "driver/gpio.h"
#include  "esp_mac.h"
#include <stdio.h>

#include "audio_processor.h"
#include "basic_board.h"

#define DEVICE_NAME_PREFIX "esp32_"

volc_hal_context_t* global_context = NULL;

basic_board_periph_t global_periph = {0};

static void __gmf_audio_init()
{
    audio_manager_config_t config = DEFAULT_AUDIO_MANAGER_CONFIG();
    basic_board_init(&global_periph);
    config.play_dev = global_periph.play_dev;
    config.rec_dev = global_periph.rec_dev;
    strcpy(config.mic_layout, global_periph.mic_layout);
    config.board_sample_rate = global_periph.sample_rate;
    config.board_bits = global_periph.sample_bits;
    config.board_channels = global_periph.channels;
    audio_manager_init(&config);
}

static void __generate_device_name(char* device_name){
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    // printf("WIFI_STA MAC 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x \n",
    //           mac[0], mac[1], mac[2],
    //           mac[3], mac[4], mac[5]);
    snprintf(device_name,VOLC_DEVICE_NAME_LENGTH,"%s%02X%02X%02X%02X%02X%02X",DEVICE_NAME_PREFIX,mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int volc_hal_init(void) {
    if(global_context == NULL){
        global_context = (volc_hal_context_t*) volc_osal_calloc(1,sizeof(volc_hal_context_t));
    }
    __gmf_audio_init();
    volc_hal_file_system_init();
    __generate_device_name(global_context->device_name);
    return 0;
}

volc_hal_context_t* volc_get_global_hal_context(){
    return global_context;
}

void volc_hal_fini(void) {
    volc_osal_free(global_context);
    global_context = NULL;
    return;
}
