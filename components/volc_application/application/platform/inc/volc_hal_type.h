// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#ifndef __VOLC_HAL_TYPE_H__
#define __VOLC_HAL_TYPE_H__

#include "volc_conv_ai.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Media type enumeration
 */
typedef enum volc_media_type {
    VOLC_MEDIA_TYPE_AUDIO = 0, // Audio media type
    VOLC_MEDIA_TYPE_VIDEO,     // Video media type
    VOLC_MEDIA_TYPE_MAX,       // Maximum media type count
} volc_media_type_e;

#ifdef __cplusplus
}
#endif

#endif