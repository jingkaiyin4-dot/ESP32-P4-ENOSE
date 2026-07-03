// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>

#include "aios.h"
#include "volc_hal.h"
#include "volc_osal.h"
#include "volc_hal_display.h"
#include "volc_hal_file.h"

#include "volc_service_common.h"
#include "volc_conv_service.h"
#include "volc_conv.h"

#if __cplusplus
extern "C" {
#endif

typedef struct volc_conv_service_manager {
    aios_session_t super;
    volc_osal_tid_t conv_thread_id;
} volc_conv_service_manager_t;

volc_conv_service_manager_t volc_conv_service_manager;

static aios_ret_t __state_conversation(volc_conv_service_manager_t * const me, aios_event_t const * const e)
{
    volc_hal_context_t* g_hal_context = volc_get_global_hal_context();
    if (g_hal_context == NULL) {
        return AIOS_Ret_NotHandled;
    }
    volc_hal_display_t global_display = g_hal_context->display_handle;
    switch (e->id) {
        case VOLC_SERVICE_AI_CONVERSATION_START:
            if (me->conv_thread_id == NULL) {
                volc_osal_thread_param_t param = {0};
                volc_hal_display_set_content(global_display,VOLC_DISPLAY_OBJ_STATUS,VOLC_DISPLAY_TEXT,"ai对话启动中");
                snprintf(param.name, sizeof(param.name), "%s", "conv_ai");
                param.stack_size = 8*1024;
                param.priority = 5;
                param.stack_in_ext = 1;
                volc_osal_thread_create(&me->conv_thread_id, &param, conv_ai_service_task,NULL);
            }
            return AIOS_Ret_Handled;
        case VOLC_SERVICE_AI_CONVERSATION_INTERRUPT:
            return AIOS_Ret_Handled;
        // control the Ai_Conversation task quit
        case VOLC_SERVICE_AI_CONVERSATION_QUIT:
            if (me->conv_thread_id != NULL) {
                conv_ai_service_task_stop();
                me->conv_thread_id = NULL;
            }          
            return AIOS_Ret_Handled;
        // notice the Ai_Conversation over by task
        case VOLC_SERVICE_AI_CONVERSATION_OVER:
            if (me->conv_thread_id != NULL) {
                me->conv_thread_id = NULL;
            }          
            return AIOS_Ret_Handled;
        default:
            return AIOS_Ret_NotHandled;
    }
}

static aios_ret_t __state_init(volc_conv_service_manager_t * const me, aios_event_t const * const e)
{
    AIOS_EVENT_SUB(VOLC_SERVICE_AI_CONVERSATION_START); // 订阅事件
    AIOS_EVENT_SUB(VOLC_SERVICE_AI_CONVERSATION_INTERRUPT); // 订阅事件
    AIOS_EVENT_SUB(VOLC_SERVICE_AI_CONVERSATION_QUIT); // 订阅事件
    AIOS_EVENT_SUB(VOLC_SERVICE_AI_CONVERSATION_OVER); // 订阅事件
    me->conv_thread_id = NULL;
    conv_ai_service_init();
    return AIOS_TRAN(__state_conversation);
}

void volc_conv_service_manager_init(void)
{
    aios_session_init(&volc_conv_service_manager.super, VOLC_SERVICE_PRIORITY_AI_CONVERSATION, NULL);
    aios_session_start(&volc_conv_service_manager.super, AIOS_STATE_CAST(__state_init));
}

#if __cplusplus
}
#endif
