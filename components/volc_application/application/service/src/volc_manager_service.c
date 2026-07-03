// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0
#include "volc_hal.h"
#include "volc_manager_service.h"
#include "volc_conv_service.h"
#include "volc_service_common.h"

typedef struct volc_service_manager {
    aios_session_t super;
} volc_service_manager_t;

volc_service_manager_t volc_service_manager;

static aios_ret_t __state_wait_service_event(volc_service_manager_t* const me, aios_event_t const * const e)
{
    volc_hal_context_t* g_hal_context = volc_get_global_hal_context();
    if(g_hal_context == NULL){
        return AIOS_Ret_NotHandled;
    }

    switch ((int)e->id) {
        case VOLC_SERVICE_NETWORK_CONFIG:
            return AIOS_Ret_Handled;
        case VOLC_SERVICE_AI_CONVERSATION:
            aios_event_pub(VOLC_SERVICE_AI_CONVERSATION_START, NULL, NULL);
            return AIOS_Ret_Handled;
        default:
            return AIOS_Ret_NotHandled;
    }
}

static aios_ret_t __state_init(volc_service_manager_t* const me, aios_event_t const * const e)
{
    AIOS_EVENT_SUB(VOLC_SERVICE_NETWORK_CONFIG); // 订阅事件
    AIOS_EVENT_SUB(VOLC_SERVICE_AI_CONVERSATION); // 订阅事件
    return AIOS_TRAN(__state_wait_service_event);
}

void volc_service_manager_init(void)
{
    aios_session_init(&volc_service_manager.super, VOLC_SERVICE_PRIORITY_APP_MANAGER, NULL);
    aios_session_start(&volc_service_manager.super, AIOS_STATE_CAST(__state_init));
}
