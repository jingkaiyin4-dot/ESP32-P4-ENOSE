#pragma once

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODEL_CHUNK_SIZE  100
#define MODEL_CMD_TIMEOUT 5000

enum ModelCmdState {
    MCS_IDLE,
    MCS_DOWNLOADING,
    MCS_SEND_META,
    MCS_SENDING_CHUNKS,
    MCS_WAIT_DONE,
    MCS_SENDING_NORM,
    MCS_SENDING_CLASSES
};

struct ModelUpdateCtx {
    char model_name[64];
    char model_version[32];
    char model_url[256];
    char norm_url[256];
    int total_chunks;
    int total_size;
    int current_chunk;
    uint8_t *file_data;
    ModelCmdState state;
    uint32_t last_action_time;
    int retry_count;
    char cmd_id[64];
};

extern float g_model_min[16];
extern float g_model_max[16];
extern char g_model_classes[8][32];
extern int g_model_num_classes;
extern bool g_model_has_norm;

bool model_mgr_load_norm_json(const char *model_name);
void model_mgr_trigger_update(const char *url, const char *norm_url, const char *name, const char *version, const char *cmd_id);
void model_mgr_handle_s3_msg(const char *mt_str, int mid);
void model_mgr_check_timeout(void);

inline void bytes_to_hex(const uint8_t *src, size_t src_len, char *dst)
{
    for (size_t i = 0; i < src_len; i++) {
        sprintf(dst + (i * 2), "%02X", src[i]);
    }
    dst[src_len * 2] = '\0';
}

struct ModelMgrStateInfo {
    char name[64];
    char version[32];
    int progress_percent;
    int state; // ModelCmdState enum
    int total_size;
};

void model_mgr_get_state(struct ModelMgrStateInfo *info);
bool model_mgr_load_and_send_sd_model(const char *model_name);
bool model_mgr_delete_local_model(const char *model_name);

#ifdef __cplusplus
}
#endif
