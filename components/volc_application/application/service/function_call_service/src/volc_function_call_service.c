// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#include <string.h>

#include "cJSON.h"

#include "volc_service_common.h"
#include "volc_conv.h"
#include "volc_function_call_service.h"
#include "volc_local_function_list.h"
#include "aios.h"
#include <stdio.h>
#if __cplusplus
extern "C" {
#endif

typedef struct volc_function_call_service {
    aios_session_t super;
} volc_function_call_service_t;

volc_function_call_service_t g_volc_function_call_service;

static void __on_function_calling_message_received(const cJSON *root)
{
    /*
        {
            "subscriber_user_id" : "",
            "tool_calls" : 
            [
                {
                    "function" : 
                    {
                        "arguments" : "{\\"location\\": \\"\\u5317\\u4eac\\u5e02\\"}",
                        "name" : "get_current_weather"
                    },
                    "id" : "call_py400kek0e3pczrqdxgnb3lo",
                    "type" : "function"
                }
            ]
        }
    */

    cJSON *tool_obj_arr = cJSON_GetObjectItem(root, "tool_calls");
    cJSON *obji = NULL;
    cJSON_ArrayForEach(obji, tool_obj_arr)
    {
        cJSON *id_obj = cJSON_GetObjectItem(obji, "id");
        cJSON *function_obj = cJSON_GetObjectItem(obji, "function");
        if (id_obj && function_obj)
        {
            cJSON *arguments_obj = cJSON_GetObjectItem(function_obj, "arguments");
            cJSON *name_obj = cJSON_GetObjectItem(function_obj, "name");
            char *arguments_json_str = cJSON_GetStringValue(arguments_obj);
            const char *func_name = cJSON_GetStringValue(name_obj);
            const char *func_id = cJSON_GetStringValue(id_obj);
            if (strcmp(func_name, "adjust_audio_val") == 0 && arguments_json_str)
            {
                cJSON *arguments_json = cJSON_Parse(arguments_json_str);
                cJSON *action_obj = cJSON_GetObjectItem(arguments_json, "action");
                const char *action = (action_obj->valuestring);
                adjust_audio_val(action);
                volc_conv_service_t conv_service = get_conv_ai_service();
                cJSON *fc_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(fc_obj, "ToolCallID", func_id);
                cJSON_AddStringToObject(fc_obj, "Content", "音量已经调整了");
                char *json_string = cJSON_Print(fc_obj);
                static char fc_message_buffer[256] = {'f', 'u', 'n', 'c'};
                int json_str_len = strlen(json_string);
                fc_message_buffer[4] = (json_str_len >> 24) & 0xff;
                fc_message_buffer[5] = (json_str_len >> 16) & 0xff;
                fc_message_buffer[6] = (json_str_len >> 8) & 0xff;
                fc_message_buffer[7] = (json_str_len >> 0) & 0xff;
                memcpy(fc_message_buffer + 8, json_string, json_str_len);
                cJSON_Delete(fc_obj);
                volc_message_info_t info = {0};
                info.is_binary = 1;
                volc_send_message(conv_service.engine, fc_message_buffer, json_str_len + 8, &info);
            }
        }
    }
    cJSON_Delete(root);
}

void  __on_function_calling_trigger(const cJSON *root)
{
/*
{
    "event_type": "function_calling",   // 事件类型，固定为此值
    "function": "get_current_weather",  // 被触发的函数名，与您在 StartVoiceChat 中定义的名称一致
    "tool_call_id": "call_12345",       // 本次函数调用的唯一ID，后续回传结果时需要用到
    "response_id": "resp_abcde"         // 本次响应的唯一ID，可用于问题排查
}
*/  
    cJSON *event_type_obj = cJSON_GetObjectItemCaseSensitive(root, "event_type");
    if(event_type_obj && strcmp(event_type_obj->valuestring, "function_calling") == 0 ) {
        cJSON *function_obj = cJSON_GetObjectItemCaseSensitive(root, "function");
        if(function_obj && strcmp(function_obj->valuestring, "adjust_audio_val") == 0){
            volc_conv_service_t conv_service = get_conv_ai_service();
            volc_send_text_to_agent(conv_service.engine, "别着急，我来给你调整音量", VOLC_AGENT_TYPE_TTS,2);
        }
    }
    cJSON_Delete(root);
}


static aios_ret_t __state_conversation(volc_function_call_service_t * const me, aios_event_t const * const e)
{
    switch (e->id) {
        case VOLC_FUNCTION_CALL_EXEC:
            __on_function_calling_message_received((const cJSON *)e->data);
            return AIOS_Ret_Handled;
        case VOLC_FUNCTION_CALL_TRIGGER:
            __on_function_calling_trigger((const cJSON *)e->data);
            return AIOS_Ret_Handled;
    }
}

static aios_ret_t __state_init(volc_function_call_service_t * const me, aios_event_t const * const e)
{
    AIOS_EVENT_SUB(VOLC_FUNCTION_CALL_EXEC); // 订阅事件
    AIOS_EVENT_SUB(VOLC_FUNCTION_CALL_TRIGGER); // 订阅事件

    return AIOS_TRAN(__state_conversation);
}

void function_call_service_init(void){
    aios_session_init(&g_volc_function_call_service.super, VOLC_SERVICE_PRIORITY_FUNCTION_CALL, NULL);
    aios_session_start(&g_volc_function_call_service.super, AIOS_STATE_CAST(__state_init));
}

#if __cplusplus
}
#endif
