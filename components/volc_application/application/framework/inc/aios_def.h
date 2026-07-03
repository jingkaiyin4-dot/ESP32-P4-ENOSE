// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#ifndef AIOS_DEF_H__
#define AIOS_DEF_H__

#include "aios_config.h"

#define AIOS_MAX_SLEEP_MS                (1000 * 10)

typedef enum aios_event_id {
    // 系统事件
    Event_Null = 0,
    Event_Enter,
    Event_Exit,
    // 用户事件
    Event_User,
} aios_event_id_t;

#endif

