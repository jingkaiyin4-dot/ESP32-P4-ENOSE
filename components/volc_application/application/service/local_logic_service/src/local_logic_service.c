// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0
#include <string.h>

#include "volc_conv_ai.h"

#include "volc_service_common.h"
#include "aios.h"
#include "volc_conv.h"
#include "volc_hal.h"
#include "volc_hal_file.h"
#include "volc_hal_player.h"

#if __cplusplus
extern "C" {
#endif

static const char *welcome_audio_file_name = "/sdcard/wel.pcm";
typedef struct volc_local_logic_service {
    aios_session_t super;
} volc_local_logic_service_t;

volc_local_logic_service_t g_volc_local_logic_service;
static int has_quit = 0;
static void __play_welcome(){
    volc_hal_context_t* g_hal_context = volc_get_global_hal_context();
    if(g_hal_context == NULL) return;
    volc_hal_player_t player =  g_hal_context->player_handle[VOLC_HAL_PLAYER_AUDIO];
    volc_hal_file_t* audioFile = volc_hal_file_open(welcome_audio_file_name, "rb");
    if(audioFile == NULL) return;
    char audio_data[160];
    if(player){
        int offset = 0;
        uint64_t size = volc_hal_file_get_size(audioFile);
        while(offset < size - 160){
            volc_hal_file_read(audioFile,audio_data,160);
            volc_hal_player_play_data(player, audio_data, 160);
            offset += 160;
        }
    }
    volc_hal_file_close(audioFile);
    return;
}

static int __has_substring(const char *haystack, const char *needle) {
    // 处理边界：子串为空串，按惯例返回包含（可根据需求调整）
    if (needle[0] == '\0') {
        return 1;
    }
    // strstr 返回非 NULL 表示找到子串
    return strstr(haystack, needle) != NULL ? 1 : 0;
}

static void __parser_and_process_subtitle(char *subtitle)
{
    volc_conv_service_t conv_service = get_conv_ai_service();

    if (__has_substring(subtitle, "退下吧") || __has_substring(subtitle, "拜拜"))
    {
        if (!has_quit)
        {
            volc_send_text_to_agent(conv_service.engine, "好的拜拜", VOLC_AGENT_TYPE_TTS, 1);
            has_quit = 1;
        }
    }
    if (__has_substring(subtitle, "好的拜拜"))
    {
        aios_event_pub(VOLC_SERVICE_AI_CONVERSATION_QUIT, NULL, NULL);
        has_quit = 0;
    }
}

static aios_ret_t __state_conversation(volc_local_logic_service_t * const me, aios_event_t const * const e)
{
    switch (e->id) {
        case VOLC_LOCAL_LOGIC_PLAY_WELCOME:
            __play_welcome();
            return AIOS_Ret_Handled;
        case VOLC_LOCAL_LOGIC_PROCESS_SUBTITLE:
            __parser_and_process_subtitle(e->data);
            return AIOS_Ret_Handled;
    }
}

static aios_ret_t __state_init(volc_local_logic_service_t * const me, aios_event_t const * const e)
{
    AIOS_EVENT_SUB(VOLC_LOCAL_LOGIC_PLAY_WELCOME); // 订阅事件
    AIOS_EVENT_SUB(VOLC_LOCAL_LOGIC_PROCESS_SUBTITLE); // 订阅事件
    return AIOS_TRAN(__state_conversation);
}

void local_logic_service_init(void){
    aios_session_init(&g_volc_local_logic_service.super, VOLC_SERVICE_PRIORITY_LOCAL_LOGIC, NULL);
    aios_session_start(&g_volc_local_logic_service.super, AIOS_STATE_CAST(__state_init));
}

#if __cplusplus
}
#endif
