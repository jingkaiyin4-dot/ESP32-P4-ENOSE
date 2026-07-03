// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0
#ifndef VOLC_CONV_COMMON_H__
#define VOLC_CONV_COMMON_H__
#include "aios.h"

enum {
    VOLC_SERVICE_EVENT_TEST = Event_User,
    VOLC_SERVICE_NETWORK_CONFIG,
    VOLC_SERVICE_AI_CONVERSATION,
    
    // Ai_Conversation event:
    VOLC_SERVICE_AI_CONVERSATION_START = 100,
    VOLC_SERVICE_AI_CONVERSATION_INTERRUPT = 101,
    VOLC_SERVICE_AI_CONVERSATION_QUIT = 102,  // control the Ai_Conversation task quit
    VOLC_SERVICE_AI_CONVERSATION_OVER = 103,  // notice the Ai_Conversation over by task

    // function_call event:
    VOLC_FUNCTION_CALL_EXEC = 200,
    VOLC_FUNCTION_CALL_TRIGGER = 201,


    // local logic event:
    VOLC_LOCAL_LOGIC_PLAY_WELCOME = 300,
    VOLC_LOCAL_LOGIC_PROCESS_SUBTITLE = 301,


    VOLC_SERVICE_EVENT_MAX,
};

enum {
    VOLC_SERVICE_PRIORITY_AI_CONVERSATION = 0,
    VOLC_SERVICE_PRIORITY_FUNCTION_CALL,
    VOLC_SERVICE_PRIORITY_LOCAL_LOGIC,
    VOLC_SERVICE_PRIORITY_APP_MANAGER,
};

#endif /* VOLC_CONV_COMMON_H__ */
