// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#ifndef VOLC_CONV_SERVICE_H__
#define VOLC_CONV_SERVICE_H__

#include "volc_conv_ai.h"
typedef struct {
    volc_event_handler_t volc_event_handler;
    volc_engine_t engine;
    int wait_time;
} volc_conv_service_t;

void conv_ai_service_task(void *pvParameters);
void conv_ai_service_task_stop();
void conv_ai_service_init();
volc_conv_service_t get_conv_ai_service();

#endif /* VOLC_CONV_SERVICE_H__ */
