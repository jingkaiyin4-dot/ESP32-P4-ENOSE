#include "volc_local_function_list.h"
#include "volc_hal.h"
#include "volc_hal_player.h"

#include <string.h>

void __volc_up_audio_val()
{
    volc_hal_context_t *g_hal_context = volc_get_global_hal_context();
    if (g_hal_context == NULL)
    {
        return;
    }
    int volc_audio_vol = volc_hal_get_audio_player_volume(g_hal_context->player_handle[VOLC_HAL_PLAYER_AUDIO]);
    volc_audio_vol = (volc_audio_vol + 10) <= 100 ? (volc_audio_vol + 10) : 100;
    volc_hal_set_audio_player_volume(g_hal_context->player_handle[VOLC_HAL_PLAYER_AUDIO], volc_audio_vol);
}

void __volc_down_audio_val()
{
    volc_hal_context_t *g_hal_context = volc_get_global_hal_context();
    if (g_hal_context == NULL)
    {
        return;
    }
    int volc_audio_vol = volc_hal_get_audio_player_volume(g_hal_context->player_handle[VOLC_HAL_PLAYER_AUDIO]);
    volc_audio_vol = (volc_audio_vol - 10) <= 0 ? 0 : (volc_audio_vol - 10);
    volc_hal_set_audio_player_volume(g_hal_context->player_handle[VOLC_HAL_PLAYER_AUDIO], volc_audio_vol);
}

void adjust_audio_val(const char *action)
{
    if (strcmp(action, "up") == 0)
    {
        __volc_up_audio_val();
    }
    else if (strcmp(action, "down") == 0)
    {
        __volc_down_audio_val();
    }
}
