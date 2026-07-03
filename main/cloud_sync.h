#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "uart_receiver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRAIN_STATUS_IDLE = 0,
    TRAIN_STATUS_QUEUED,
    TRAIN_STATUS_TRAINING,
    TRAIN_STATUS_SIMULATING,
    TRAIN_STATUS_COMPLETED,
    TRAIN_STATUS_FAILED,
    TRAIN_STATUS_CANCELLED,
    TRAIN_STATUS_TIMEOUT
} train_status_t;

typedef struct {
    bool active;
    char training_id[40];
    char model_name[48];
    train_status_t status;
    char phase[64];
    int progress;
    int epoch;
    int total_epochs;
    float accuracy;
    float loss;
    char error[128];
    uint32_t ts;
} train_info_t;

// Global training state
extern train_info_t g_train_info;
extern void *g_train_info_mutex;

/* ── P4 UI aggregate status (backend AI optimization) ── */
typedef struct {
    bool valid;                 /* whether a successful fetch has occurred */
    bool cloud_online;
    char ai_state[24];          /* enum: idle / collecting_data / tuning_model ... */
    char ai_state_text[48];     /* short Chinese text from server */
    int  ai_progress;           /* 0-100 */
    char current_task[96];
    int  sample_today;
    int  sample_valid;
    char data_quality_text[32];
    char model_status_text[48];
    int  model_progress;
    char storage_plan_text[48];
    char latest_plan[96];
    char last_action[96];
    bool ai_online;
    uint32_t ai_last_trigger_ts;
    char ai_last_reason[128];
    int ai_trigger_count;
    bool ai_uv_on;
    bool ai_fog_on;
    bool ai_fan_on;
    bool ai_lid_on;
    bool ai_take_photo;
    uint32_t updated_at;
} p4_ui_status_t;

extern p4_ui_status_t g_p4_ui_status;
extern void *g_p4_ui_status_mutex;

/* Fetch the aggregate UI status from server (called by cloud task).
 * On failure, falls back to local inference so the UI still shows something. */
void cloud_sync_fetch_p4_ui_status(void);

/* Fetch the AI auto-control status with per-peripheral trigger state. */
void cloud_sync_fetch_ai_control_status(void);

void cloud_sync_init(void);
void cloud_sync_set_enabled(bool enabled);
bool cloud_sync_is_enabled(void);
void cloud_sync_trigger_once(void);
void cloud_sync_post_c6_data(const ble_sensor_data_t *data);
void cloud_sync_trigger_model_info_upload(void);
void cloud_sync_trigger_model_list_upload(void);
void cloud_sync_trigger_model_training(void);

void cloud_sync_fetch_csv_list(void);
void cloud_sync_start_remote_training(const char **csv_files, int file_count, const char *model_name, int target_accuracy);

void cloud_sync_start_train_feed_poll(void);
void cloud_sync_stop_train_feed_poll(void);
void cloud_sync_clear_training_state(void);

#define MAX_CSV_FILES 30
extern char g_csv_filenames[MAX_CSV_FILES][128];
extern int g_csv_file_samples[MAX_CSV_FILES];
extern int g_csv_file_count;
extern volatile int g_csv_fetch_state; // 0=idle, 1=fetching, 2=success, 3=failed

#ifdef __cplusplus
}
#endif
