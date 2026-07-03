// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#ifndef __VOLC_HAL_FILE_H__
#define __VOLC_HAL_FILE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * File systems may have different implementations on different devices,
 * but we generally adhere to the ANSI C style.
 * For devices without a file system, these interfaces may be left unimplemented
 * as this is not mandatory.
 */

/**
 * @brief file instance pointer
 */
typedef void* volc_hal_file_t;

/**
 * @brief init the file system
 * @return int 0 if success, otherwise error code
 */
int  volc_hal_file_system_init();

/**
 * @brief open a file instance
 * @param file_name the file name
 * @param mode openmode, just follow ANSI C style(fopen)
 * @return volc_hal_file_t file instance pointer
 */
volc_hal_file_t volc_hal_file_open(char* file_name, char* mode);

/**
 * @brief read the data from the file
 * @param file the file instance pointer
 * @param data the data pointer
 * @param size the data size 
 * @return return the real read data size 
 */
uint64_t volc_hal_file_read(volc_hal_file_t file, void *data, uint64_t size);

/**
 * @brief write the data from the file
 * @param file the file instance pointer
 * @param data the data pointer
 * @param size the data size 
 * @return return the real writed data size 
 */
uint64_t volc_hal_file_write(volc_hal_file_t file, void *data, uint64_t size);

/**
 * @brief get the file size
 * @param file the file instance pointer
 * @return return the file size 
 */
uint64_t volc_hal_file_get_size(volc_hal_file_t file);

/**
 * @brief get the file size
 * @param file the file instance pointer
 * @param offset the file offset (start is 0)
 * @return int 0 if success, otherwise error code
 */
int volc_hal_file_seek(volc_hal_file_t file,uint64_t offset);

/**
 * @brief close a file instance
 * @param mode openmode, just follow ANSI C style(fopen)
 * @return volc_hal_file_t file instance pointer
 */
int volc_hal_file_close(volc_hal_file_t file);

#ifdef __cplusplus
}
#endif

#endif