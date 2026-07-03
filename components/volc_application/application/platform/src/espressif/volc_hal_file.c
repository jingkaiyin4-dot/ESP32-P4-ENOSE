// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#include <stdio.h>

#include "volc_hal_file.h"
// #include "board.h"
// #include "esp_peripherals.h"
#include "util/volc_log.h"
#include "basic_board.h"
#include "audio_processor.h"

int volc_hal_file_system_init()
{
    return 0;
}

volc_hal_file_t volc_hal_file_open(char* file_name, char* mode)
{
    FILE* file = fopen(file_name,mode);
    if (file == NULL) {
        LOGE("volc_hal_file_open failed");
        return NULL;
    }
    return (volc_hal_file_t)file;
}

uint64_t volc_hal_file_read(volc_hal_file_t file, void *data, uint64_t size)
{
    return fread(data,1,size,(FILE*)file);
}

uint64_t volc_hal_file_write(volc_hal_file_t file, void *data, uint64_t size)
{
    return fwrite(data,1,size,(FILE*)file);
}

uint64_t volc_hal_file_get_size(volc_hal_file_t file)
{
    uint64_t file_offset = ftell((FILE*)file);
    fseek((FILE*)file, 0, SEEK_END);
    uint64_t file_size_long = ftell((FILE*)file);
    fseek((FILE*)file,file_offset,SEEK_SET);
    return file_size_long;
}

int volc_hal_file_seek(volc_hal_file_t file,uint64_t offset)
{
    return fseek((FILE*)file,offset,SEEK_SET);
}

int volc_hal_file_close(volc_hal_file_t file)
{
    return fclose((FILE*)file);
}