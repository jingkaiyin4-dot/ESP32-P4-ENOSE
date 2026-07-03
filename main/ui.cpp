#include "ui.h"
#include "lvgl.h"
// #include "font/binfont_loader/lv_binfont_loader.h" // Removed SD card binary font loader include for Flash-only version
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "app_video.h"
#include "app_system.h"
#include "quirc.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "esp_private/esp_cache_private.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "bsp/esp-bsp.h"
#include "bme680.h"
#include "ble_central.h"


#include <vector>
#include <string>
#include <algorithm>
#include "model_mgr.h"
#include "uart_receiver.h"
#include "cloud_sync.h"
#include "xiaozhi_ai_service.h"

#ifdef CONFIG_ELEC_NOSE_GATEWAY_ENABLE
#include "electronic_nose_gateway.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <dirent.h>

extern "C" {
    esp_err_t bsp_extra_codec_volume_set(int volume, void* handle);
    esp_err_t bsp_display_brightness_set(int brightness_percent);
    void web_jpeg_encode_frame(const uint8_t *cam_buf, uint32_t w, uint32_t h);
    bool web_jpeg_available(void);
    bool web_jpeg_get_frame(uint8_t **buf, uint32_t *w, uint32_t *h, size_t *len);
    /* app_video resolution override */
    extern uint32_t g_video_req_width;
    extern uint32_t g_video_req_height;
}

// ===== 静态 IP 配置 =====
#define P4_STATIC_IP       "192.168.110.100"
#define P4_GATEWAY         "192.168.110.1"
#define P4_NETMASK         "255.255.255.0"
#define P4_DNS             "114.114.114.114"
#define P4_DNS_BACKUP      "223.5.5.5"

LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_32);
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_source_han_sans_sc_14_cjk);
LV_FONT_DECLARE(ui_font_xiaozhi_14);

#include "sd_card_bg.h"

static lv_font_t s_font_cjk_fallback;

// Xiaozhi AI chat screen globals
static lv_obj_t * xiaozhi_chat_panel = NULL;
static lv_obj_t * xiaozhi_chat_label = NULL;
static lv_timer_t * s_typewriter_timer = NULL;
static int s_typewriter_char_idx = 0;
static char s_typewriter_current_buf[512] = "";
static char s_xiaozhi_target_text[512] = "";

static lv_obj_t * scr_startup;
static lv_obj_t * scr_main;
static lv_obj_t * btn_wifi;
static lv_obj_t * wifi_status_label;
static lv_obj_t * cc_overlay;
static lv_obj_t * cc_wifi_status_label;
static lv_obj_t * cc_volume_slider;
static lv_obj_t * cc_volume_label;
static lv_obj_t * cc_brightness_slider;
static bool cc_is_open = false;
static lv_obj_t * apps_panel;
static bool apps_drawer_open = false;
static lv_obj_t * side_mem_arc = NULL;
static lv_obj_t * side_mem_lbl = NULL;
static lv_obj_t * side_temp_lbl = NULL;
static lv_obj_t * side_humi_lbl = NULL;

static lv_timer_t * fake_data_timer;

static lv_obj_t * c6_status_label;
static lv_obj_t * c6_status_dot;

static lv_obj_t * sync_status_label;
static lv_obj_t * sync_status_dot;
static lv_obj_t * wifi_tile;
static lv_obj_t * c6_tile;
static lv_obj_t * sync_tile;

static lv_obj_t * wifi_list_modal;
static lv_obj_t * wifi_pass_modal;
static lv_obj_t * wifi_pass_title;
static lv_obj_t * ta_pass;
static lv_obj_t * kb;
static char selected_ssid[64] = "";
static lv_obj_t * wifi_list;
static bool wifi_initialized = false;

static lv_obj_t * s_wallpaper_img = NULL;
static char s_current_wallpaper_path[128] = "";
static lv_obj_t * s_startup_wallpaper_img = NULL;
static char s_current_startup_wallpaper_path[128] = "";
static lv_obj_t * wallpaper_modal;
static lv_obj_t * wallpaper_list;

typedef enum {
    WALLPAPER_TARGET_STARTUP,
    WALLPAPER_TARGET_MAIN
} wallpaper_target_t;
static wallpaper_target_t s_wallpaper_target = WALLPAPER_TARGET_MAIN;

/* Island instances for startup and main screens */
struct island_t {
    lv_obj_t * capsule;
    lv_obj_t * led_wrapper;
    lv_obj_t * led_glow;
    lv_obj_t * led_core;
    lv_obj_t * aurora_glow;   /* Siri-style multicolor aurora halo */
    lv_obj_t * status_lbl;
    lv_obj_t * details_cnt;
    lv_obj_t * telemetry_title;
    lv_obj_t * detail_lbls[5];
    
    // AI Dialog UI elements
    lv_obj_t * ai_title;
    lv_obj_t * ai_chat_lbl;
    lv_obj_t * ai_status_lbl;
    lv_obj_t * ai_progress_bar;
    lv_obj_t * ai_progress_lbl;
    
    bool expanded;
};

static island_t s_island = {0};
static island_t m_island = {0};

static lv_timer_t * s_startup_check_timer = NULL;
static lv_timer_t * s_island_flow_timer = NULL;
static int s_startup_check_index = 0;

/* Startup neon art text globals */
static lv_obj_t * s_neon_container = NULL;
static lv_obj_t * s_neon_aura = NULL;
static lv_obj_t * s_neon_glow = NULL;
static lv_obj_t * s_neon_core = NULL;

static lv_obj_t * scr_qr;
static lv_obj_t * cam_img_obj;
static lv_obj_t * qr_result_panel;
static lv_obj_t * qr_result_label;

static char s_ai_result_buffer[4096] = {0};

#ifdef CONFIG_ELEC_NOSE_GATEWAY_ENABLE
static lv_obj_t * ai_analyze_btn;
static lv_obj_t * ai_analyze_btn_label;
static lv_obj_t * ai_result_dialog;
static lv_obj_t * ai_result_dialog_text;
static bool s_ai_dialog_open = false;
#endif

static lv_obj_t * siri_state_label = NULL;
bool ai_service_running = false;
static bool ptt_pressed = false;
bool g_temp_tts_active = false;
uint32_t g_temp_tts_start_time_ms = 0;

/* Dialogue & Microphone control panel widgets */
static lv_obj_t * ai_control_panel = NULL;
static lv_obj_t * btn_dialogue_toggle = NULL;
static lv_obj_t * btn_mic_toggle = NULL;
static lv_obj_t * lbl_dialogue_btn = NULL;
static lv_obj_t * lbl_mic_btn = NULL;
static lv_obj_t * ai_training_progress_bar = NULL;
static lv_obj_t * ai_training_progress_label = NULL;


/* UV button references for ui_set_uv() */
static lv_obj_t *s_uv_btn = NULL;
static lv_obj_t *s_uv_lbl = NULL;
static lv_obj_t *s_warmup_lbl = NULL;
static lv_obj_t *s_uv_remain_lbl = NULL;
static lv_obj_t *s_uv_dur_lbl = NULL;
static int s_uv_dur_setting = 30;  /* local UV duration tracking (seconds) */

/* Fogger & Fan button references */
static lv_obj_t *s_fog_btn = NULL;
static lv_obj_t *s_fog_lbl = NULL;
static lv_obj_t *s_fan_btn = NULL;
static lv_obj_t *s_fan_lbl = NULL;
static lv_obj_t *s_lid_btn = NULL;
static lv_obj_t *s_lid_lbl = NULL;

/* Cloud AI Auto-Control references */
bool g_cloud_ai_auto = false;
static lv_obj_t *s_cloud_ai_btn = NULL;
static lv_obj_t *s_cloud_ai_lbl = NULL;

static void cloud_ai_update_ui_cb(void *arg)
{
    bool on = (bool)(intptr_t)arg;
    if (s_cloud_ai_btn && s_cloud_ai_lbl) {
        if (on) {
            lv_label_set_text(s_cloud_ai_lbl, "AI AUTO: ON");
            lv_obj_set_style_bg_color(s_cloud_ai_btn, lv_color_hex(0x00A86B), 0); // Jade Green
            lv_obj_set_style_bg_opa(s_cloud_ai_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(s_cloud_ai_btn, lv_color_hex(0xA7F3D0), 0);
            lv_obj_set_style_border_width(s_cloud_ai_btn, 1, 0);
            lv_obj_set_style_text_color(s_cloud_ai_lbl, lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_label_set_text(s_cloud_ai_lbl, "AI AUTO: OFF");
            lv_obj_set_style_bg_color(s_cloud_ai_btn, lv_color_hex(0x1F2A44), 0); // Actuator Slate Blue
            lv_obj_set_style_bg_opa(s_cloud_ai_btn, LV_OPA_60, 0);
            lv_obj_set_style_border_color(s_cloud_ai_btn, lv_color_hex(0x3B4F7A), 0);
            lv_obj_set_style_border_width(s_cloud_ai_btn, 1, 0);
            lv_obj_set_style_text_color(s_cloud_ai_lbl, lv_color_hex(0xFFFFFF), 0);
        }
    }
}

static void uv_update_ui_cb(void *arg)
{
    bool on = (bool)(intptr_t)arg;
    if (s_uv_btn && s_uv_lbl) {
        if (on) {
            lv_label_set_text(s_uv_lbl, "UV ON");
            lv_obj_set_style_bg_color(s_uv_btn, lv_color_hex(0x9D4EDD), 0); // Royal Violet
            lv_obj_set_style_bg_opa(s_uv_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(s_uv_btn, lv_color_hex(0xD8B4FE), 0);
            lv_obj_set_style_border_width(s_uv_btn, 1, 0);
        } else {
            lv_label_set_text(s_uv_lbl, "UV OFF");
            lv_obj_set_style_bg_color(s_uv_btn, lv_color_hex(0x1F2A44), 0); // Translucent slate
            lv_obj_set_style_bg_opa(s_uv_btn, LV_OPA_60, 0);
            lv_obj_set_style_border_color(s_uv_btn, lv_color_hex(0x3B4F7A), 0);
            lv_obj_set_style_border_width(s_uv_btn, 1, 0);
        }
    }
}

static void fog_update_ui_cb(void *arg)
{
    bool on = (bool)(intptr_t)arg;
    if (s_fog_btn && s_fog_lbl) {
        if (on) {
            lv_label_set_text(s_fog_lbl, "FOG ON");
            lv_obj_set_style_bg_color(s_fog_btn, lv_color_hex(0x00B4D8), 0); // Glowing Cyan
            lv_obj_set_style_bg_opa(s_fog_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(s_fog_btn, lv_color_hex(0x90E0EF), 0);
            lv_obj_set_style_border_width(s_fog_btn, 1, 0);
        } else {
            lv_label_set_text(s_fog_lbl, "FOG OFF");
            lv_obj_set_style_bg_color(s_fog_btn, lv_color_hex(0x1F2A44), 0);
            lv_obj_set_style_bg_opa(s_fog_btn, LV_OPA_60, 0);
            lv_obj_set_style_border_color(s_fog_btn, lv_color_hex(0x3B4F7A), 0);
            lv_obj_set_style_border_width(s_fog_btn, 1, 0);
        }
    }
}

static void fan_update_ui_cb(void *arg)
{
    bool on = (bool)(intptr_t)arg;
    if (s_fan_btn && s_fan_lbl) {
        if (on) {
            lv_label_set_text(s_fan_lbl, "FAN ON");
            lv_obj_set_style_bg_color(s_fan_btn, lv_color_hex(0x2EE59D), 0); // Emerald Green
            lv_obj_set_style_bg_opa(s_fan_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(s_fan_btn, lv_color_hex(0xA7F3D0), 0);
            lv_obj_set_style_border_width(s_fan_btn, 1, 0);
        } else {
            lv_label_set_text(s_fan_lbl, "FAN OFF");
            lv_obj_set_style_bg_color(s_fan_btn, lv_color_hex(0x1F2A44), 0);
            lv_obj_set_style_bg_opa(s_fan_btn, LV_OPA_60, 0);
            lv_obj_set_style_border_color(s_fan_btn, lv_color_hex(0x3B4F7A), 0);
            lv_obj_set_style_border_width(s_fan_btn, 1, 0);
        }
    }
}

static void lid_update_ui_cb(void *arg)
{
    bool on = (bool)(intptr_t)arg;
    if (s_lid_btn && s_lid_lbl) {
        if (on) {
            lv_label_set_text(s_lid_lbl, "LID ON");
            lv_obj_set_style_bg_color(s_lid_btn, lv_color_hex(0xFF9F40), 0); // Safety Orange
            lv_obj_set_style_bg_opa(s_lid_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(s_lid_btn, lv_color_hex(0xFFE3A8), 0);
            lv_obj_set_style_border_width(s_lid_btn, 1, 0);
        } else {
            lv_label_set_text(s_lid_lbl, "LID OFF");
            lv_obj_set_style_bg_color(s_lid_btn, lv_color_hex(0x1F2A44), 0);
            lv_obj_set_style_bg_opa(s_lid_btn, LV_OPA_60, 0);
            lv_obj_set_style_border_color(s_lid_btn, lv_color_hex(0x3B4F7A), 0);
            lv_obj_set_style_border_width(s_lid_btn, 1, 0);
        }
    }
}

/* 
 * 传感器节点数据结构
 * 说明：以前的 UDP 接收线程和端口定义已完全删除。
 * 当前数据流已全面切换为 UART 串口接收，通过 ui_handle_sensor_data 写入。
 * 此处结构体和变量名保留以作 UI 渲染的数据缓存。
 */
typedef struct {
    bool valid;
    float temp;
    float hum;
    float odor;
    float hcho;
    float co;
    float voc;
    int co2;
    int pred;
    char sensor_class[32];
    float conf;
    int fresh;
    bool uv;
    bool uv_auto;
    int uv_remain;
    int uv_dur;
    bool fog;
    bool fog_auto;
    int fog_remain;
    int fog_dur;
    bool fan;
    bool fan_auto;
    int fan_remain;
    int fan_dur;
    bool lid;
    bool lid_auto;
    int lid_remain;
    int lid_dur;
} udp_node_data_t;

static udp_node_data_t s_udp_node1 = {};
static udp_node_data_t s_udp_node2 = {};
static SemaphoreHandle_t s_udp_data_mutex = NULL;

/* ── Shared detail screen (all devices use this one screen) ── */
static lv_obj_t *scr_device_detail = NULL;
static lv_obj_t *device_detail_title = NULL;
static lv_obj_t *d_voc_label = NULL;
static lv_obj_t *d_co2_label = NULL;
static lv_obj_t *d_odor_label = NULL;
static lv_obj_t *d_hcho_label = NULL;
static lv_obj_t *d_co_label = NULL;
static lv_obj_t *d_pred_label = NULL;
static int s_current_device_idx = -1;

/* Chart for real-time telemetry curve */
static lv_obj_t *s_detail_chart = NULL;
static lv_chart_series_t *s_chart_ser_voc = NULL;

/* ── Dynamic device data + sidebar buttons ── */
#define MAX_DEVICE_APPS 12
typedef struct {
    char name[32];
    udp_node_data_t data;
    bool active;
    lv_obj_t *app_btn;  /* sidebar button only — no per-device screen */
} device_app_t;
static device_app_t s_device_apps[MAX_DEVICE_APPS];
static int s_device_app_count = 0;
static SemaphoreHandle_t s_device_app_mutex = NULL;

#define SCANNED_FILE "/sdcard/scanned.txt"
#define MAX_SCANNED_DEVICES 32

static device_app_t* device_app_ensure(const char *name);
static void device_app_delete(int idx);

static void save_scanned_sd(const char *name)
{
    if (!name || !name[0]) return;
    if (!sd_card_bg_is_ready() && sd_card_bg_init() != ESP_OK) return;
    FILE *f = fopen(SCANNED_FILE, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
            if (strcmp(line, name) == 0) { fclose(f); return; }
        }
        fclose(f);
    }
    f = fopen(SCANNED_FILE, "a");
    if (!f) return;
    fprintf(f, "%s\n", name);
    fclose(f);
    ESP_LOGI("UI", "Saved scanned device to SD: %s", name);
}

static void restore_scanned_sd(void)
{
    if (!sd_card_bg_is_ready() && sd_card_bg_init() != ESP_OK) return;
    FILE *f = fopen(SCANNED_FILE, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len > 0) {
            device_app_ensure(line);
            ESP_LOGI("UI", "Restored device app from SD: %s", line);
        }
    }
    fclose(f);
}

static int video_fd = -1;
static bool camera_initialized = false;

#define CAM_BUF_NUM 2
static uint8_t *cam_buffers[CAM_BUF_NUM] = {NULL};
static size_t cam_buf_size = 0;
static size_t cam_alignment = 0;

#if LVGL_VERSION_MAJOR == 9
static lv_draw_buf_t cam_draw_buf = {};
#else
static lv_img_dsc_t cam_img_dsc = {};
#endif

static volatile int wifi_conn_status = 0;  // 0=init, 1=connecting, 2=connected, 3=disconnected
static volatile int wifi_status_timer_ticks = 0;  // timeout counter for connecting state
static TaskHandle_t wifi_scan_task_handle = NULL;
static SemaphoreHandle_t wifi_scan_mutex = NULL;
static volatile bool wifi_scan_in_progress = false;
static char wifi_scan_ssids[60][33] = {};
static uint16_t wifi_scan_result_count = 0;
static volatile bool wifi_scan_last_ok = false;
static volatile bool wifi_scan_disconnect_requested = false;
static volatile bool wifi_scan_done = false;
static bool wifi_disconnected_for_scan = false;
static volatile uint32_t wifi_got_ip_tick = 0;
static int wifi_disconnect_count = 0;

// 模型状态全局变量
extern "C" {
    extern volatile int g_training_progress;
    extern volatile int g_training_cur_epoch;
    extern volatile int g_training_tot_epoch;
}
static lv_obj_t * main_model_card = NULL;
static lv_obj_t * main_model_name_label = NULL;
static lv_obj_t * main_model_classes_label = NULL;
static lv_obj_t * main_model_status_led = NULL;
static lv_obj_t * main_model_bar = NULL;
static lv_obj_t * model_switch_modal = NULL;


// 远程训练模态框全局变量
static lv_obj_t * train_modal = NULL;
static lv_obj_t * train_csv_list = NULL;
static lv_obj_t * train_model_name_ta = NULL;
static lv_obj_t * train_accuracy_slider = NULL;
static lv_obj_t * train_accuracy_label = NULL;
static lv_obj_t * btn_start_train = NULL;
static lv_obj_t * train_list_title = NULL;
static lv_obj_t * train_name_lbl = NULL;
static lv_obj_t * train_acc_lbl = NULL;

static lv_obj_t * train_progress_cont = NULL;
static lv_obj_t * modal_training_bar = NULL;
static lv_obj_t * modal_training_label = NULL;
static lv_obj_t * modal_training_status = NULL;
static lv_obj_t * modal_model_name_val = NULL;
static lv_obj_t * modal_status_badge = NULL;
static lv_obj_t * modal_phase_val = NULL;
static lv_obj_t * modal_epoch_val = NULL;
static lv_obj_t * modal_accuracy_val = NULL;
static lv_obj_t * modal_loss_val = NULL;

static void update_volume_ui(int vol, bool sync_sliders);
static void set_wifi_status_text(const char * text, lv_color_t color);
static void control_center_init(lv_obj_t * parent);
static void main_screen_event_cb(lv_event_t * e);
static void wifi_scan_task(void * arg);
static void request_wifi_scan(void);
static void render_wifi_scan_results(void);
static void render_wifi_scan_results_async(void * user_data);
static void save_wifi_creds(const char *ssid, const char *pass);
static bool load_wifi_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
static void set_wallpaper(const char *file_path);
static void save_wallpaper_path(const char *path);
static bool load_wallpaper_path(char *path, size_t len);
static void save_startup_wallpaper_path(const char *path);
static bool load_startup_wallpaper_path(char *path, size_t len);
static void create_wallpaper_modal(void);
static void open_wallpaper_list(void);
static void open_model_switch_modal_cb(lv_event_t * e);
static void create_training_modal(void);
static void open_training_modal_cb(lv_event_t * e);
static void render_csv_list_ui(void);
static void btn_start_train_cb(lv_event_t * e);
static void train_modal_close_cb(lv_event_t * e);

/* Forward declarations for premium click and zoom animations */
static void open_modal_with_zoom_anim(lv_obj_t * modal);
static void close_modal_with_zoom_anim_delete(lv_obj_t ** modal_ptr);
static void close_modal_with_zoom_anim_hidden(lv_obj_t * modal);
static void ui_setup_premium_click_effect(lv_obj_t * obj, lv_color_t active_border_color, lv_color_t glow_color);
static void ui_setup_normal_click_effect(lv_obj_t * obj, lv_color_t active_border_color, lv_color_t glow_color);

/* Forward declarations for startup check bar callbacks */
static void build_island(lv_obj_t * parent, island_t * isl);
static island_t * active_island(void);
static void island_size_anim_cb(void * var, int32_t v);
static void island_flow_timer_cb(lv_timer_t * timer);
static void toggle_island(island_t * isl);
static void update_island_view_mode(island_t * isl);
static void update_island_details(island_t * isl);
static void neon_anim_glow_cb(void * var, int32_t v);
static void neon_anim_aura_cb(void * var, int32_t v);
static void neon_anim_glow_x_cb(void * var, int32_t v);
static void neon_anim_aura_y_cb(void * var, int32_t v);

static void ptt_event_cb(lv_event_t * e);
static void refresh_ai_status_ui(void);
static void start_aurora_animation(lv_obj_t * capsule);
static void stop_aurora_animation(lv_obj_t * capsule);
static void create_siri_orb(void);

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    EventGroupHandle_t eg = app_system_event_group();
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        ESP_LOGI("WIFI", "SCAN_DONE event received");
        wifi_scan_done = true;
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_scan_in_progress || wifi_scan_disconnect_requested) {
            wifi_scan_disconnect_requested = false;
            ESP_LOGI("WIFI", "Wi-Fi disconnected for scan");
            // Just set status flag, UI update happens in fake_data_timer_cb
            wifi_conn_status = 1;
            wifi_status_timer_ticks = 0;
            return;
        }

        wifi_event_sta_disconnected_t *discon = (wifi_event_sta_disconnected_t*)event_data;
        uint8_t reason = discon ? discon->reason : 0;
        ESP_LOGW("WIFI", "Wi-Fi disconnected, reason=%d", reason);
        wifi_disconnect_count++;
        if (wifi_disconnect_count >= 3) {
            ESP_LOGW("WIFI", "Reconnect limit reached (3), backing off to passive auto-reconnect");
            wifi_conn_status = 3;
            wifi_got_ip_tick = 0;
            if (eg) xEventGroupClearBits(eg, APP_EVENT_WIFI_CONNECTED);
            if (eg) xEventGroupSetBits(eg, APP_EVENT_WIFI_DISCONNECTED);
            wifi_disconnect_count = 0;
            // 移除了非线程安全的 set_wifi_status_text 调用。UI 状态将在 fake_data_timer_cb 线程上下文中自动安全更新。
            return;
        }
        wifi_conn_status = 3;
        wifi_got_ip_tick = 0;
        if (eg) xEventGroupClearBits(eg, APP_EVENT_WIFI_CONNECTED);
        if (eg) xEventGroupSetBits(eg, APP_EVENT_WIFI_DISCONNECTED);
        wifi_conn_status = 1;
        wifi_status_timer_ticks = 0;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("WIFI", "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_conn_status = 2;
        wifi_got_ip_tick = xTaskGetTickCount();
        wifi_status_timer_ticks = 0;
        wifi_disconnect_count = 0;
        if (eg) {
            xEventGroupSetBits(eg, APP_EVENT_WIFI_CONNECTED);
            xEventGroupClearBits(eg, APP_EVENT_WIFI_DISCONNECTED);
        }

        {
            wifi_config_t conf;
            if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
                snprintf(selected_ssid, sizeof(selected_ssid), "%s", conf.sta.ssid);
                save_wifi_creds((const char *)conf.sta.ssid, (const char *)conf.sta.password);
            }
        }
        // UI update will happen in fake_data_timer_cb (LVGL context)

        // 确保 DNS 设置生效
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_dns_info_t dns;
            if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK || dns.ip.u_addr.ip4.addr == 0) {
                inet_pton(AF_INET, P4_DNS, &dns.ip);
                esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
                inet_pton(AF_INET, P4_DNS_BACKUP, &dns.ip);
                esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns);
                ESP_LOGI("WIFI", "DNS reset: main=%s backup=%s", P4_DNS, P4_DNS_BACKUP);
            } else {
                char buf[16];
                inet_ntop(AF_INET, &dns.ip, buf, sizeof(buf));
                ESP_LOGI("WIFI", "DNS already set to %s", buf);
            }
        }
    }
}

static bool camera_display_initialized = false;
static volatile bool camera_stream_stopping = false;
static int frame_skip_counter = 0;
static bool cam_scale_applied = false;
static bool cam_header_set = false;

/* QR code scan resolution (camera is 1280x720 → downsample to this for quirc) */
#define QR_W 320
#define QR_H 180

/* Display: single buffer, update at 15fps */
#define DISP_BUF_COUNT 1
static uint8_t *disp_buffers[DISP_BUF_COUNT] = {NULL};
static int disp_buf_idx = 0;

/* QR processing buffers (320x180 grayscale = 57.6KB) */
static uint8_t *qr_gray = NULL;
static size_t qr_gray_size = 0;
static uint32_t s_cam_width = 0;
static uint32_t s_cam_height = 0;

/* QR scan task: runs quirc on core 1, never blocks camera callback */
static TaskHandle_t qr_task_handle = NULL;
static SemaphoreHandle_t qr_write_sem = NULL;  /* camera→buffer, starts at 1 */
static SemaphoreHandle_t qr_read_sem = NULL;   /* buffer→QR task, starts at 0 */
static volatile bool qr_task_running = false;
static TaskHandle_t qr_cleanup_notify_task = NULL;  /* task to notify when QR task exits */
static bool s_camera_cleanup_pending = false;
static StaticTask_t qr_task_buf;
static StackType_t *qr_task_stack = NULL;

/* Global style to disable shadows - applied to every container */
static lv_style_t g_style_no_shadow;

static void apply_no_shadow(lv_obj_t * obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &g_style_no_shadow, 0);
    lv_obj_add_style(obj, &g_style_no_shadow, LV_PART_SCROLLBAR);
    lv_obj_add_style(obj, &g_style_no_shadow, LV_PART_INDICATOR);
}

static void camera_frame_cb(uint8_t *cam_buf, uint8_t buf_index, uint32_t width, uint32_t height, size_t size) {
    LV_UNUSED(buf_index);

    if (!camera_display_initialized || camera_stream_stopping) {
        return;
    }

    web_jpeg_encode_frame(cam_buf, width, height);

    s_cam_width = width;
    s_cam_height = height;

    /* Allocate display and QR buffers on first frame */
    if (disp_buffers[0] == NULL) {
        size_t align;
        esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align);
        disp_buffers[0] = (uint8_t *)heap_caps_aligned_alloc(align, size, MALLOC_CAP_SPIRAM);
        ESP_LOGI("CAM", "Display buffer allocated: %p", disp_buffers[0]);

        /* Allocate QR gray buffer */
        size_t gray_sz = (QR_W * QR_H + align - 1) & ~(align - 1);
        qr_gray = (uint8_t *)heap_caps_aligned_alloc(align, gray_sz, MALLOC_CAP_SPIRAM);
        qr_gray_size = gray_sz;
        ESP_LOGI("QR", "QR gray buffer: %dx%d (%zu bytes)", QR_W, QR_H, gray_sz);
    }

    /* ── Display: 15fps update ── */
    if (++frame_skip_counter % 2 == 0) {
        memcpy(disp_buffers[0], cam_buf, size);
        esp_cache_msync(disp_buffers[0], size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

        if (cam_img_obj != NULL) {
            esp_err_t lock_ret = esp_lv_adapter_lock(pdMS_TO_TICKS(100));
            if (lock_ret == ESP_OK) {
#if LVGL_VERSION_MAJOR == 9
                uint32_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
                if (stride * height > size) stride = width * 2;
                lv_result_t db_ret = lv_draw_buf_init(&cam_draw_buf, width, height, LV_COLOR_FORMAT_RGB565,
                                                      stride, disp_buffers[0], size);
                if (db_ret == LV_RESULT_OK) {
                    lv_image_cache_drop(&cam_draw_buf);
                    lv_image_set_src(cam_img_obj, &cam_draw_buf);
                    lv_obj_invalidate(cam_img_obj);
                }
                esp_lv_adapter_unlock();
#else
                if (!cam_header_set) {
                    cam_img_dsc.header.always_zero = 0;
                    cam_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
                    cam_img_dsc.header.w = width;
                    cam_img_dsc.header.h = height;
                    cam_img_dsc.data_size = size;
                    cam_header_set = true;
                }
                cam_img_dsc.data = disp_buffers[0];
                lv_img_set_src(cam_img_obj, &cam_img_dsc);
                esp_lv_adapter_unlock();
#endif
            }
        }
    }

    /* ── QR: downscale + signal task (every 4th camera frame) ── */
    if (qr_gray && qr_task_handle) {
        static int qr_frame_cnt = 0;
        if (++qr_frame_cnt % 2 == 0) {
            /* Producer-consumer: only write if QR task has consumed previous frame */
            if (xSemaphoreTake(qr_write_sem, 0) != pdTRUE) {
                ESP_LOGV("QR", "QR task busy, skipping frame");
            } else {
                /* Downscale cam_buf → qr_gray (software, no PPA intermediate buffer) */
                int32_t x_step = ((int32_t)width << 16) / QR_W;
                const uint16_t *src = (const uint16_t *)cam_buf;
                uint8_t *d = qr_gray;
                for (int y = 0; y < QR_H; y++) {
                    int row_start = (y * height / QR_H) * width;
                    const uint16_t *row = src + row_start;
                    int32_t x_fp = 0;
                    for (int x = 0; x < QR_W; x++) {
                        uint16_t p = row[x_fp >> 16];
                        *d++ = ((((p >> 8) & 0xF8) * 77 +
                                 ((p >> 3) & 0xFC) * 150 +
                                 ((p << 3) & 0xF8) * 29) >> 8);
                        x_fp += x_step;
                    }
                }

                /* Flush qr_gray so QR task on core 1 sees it, then signal */
                esp_cache_msync(qr_gray, qr_gray_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
                xSemaphoreGive(qr_read_sem);
            }
        }
    }
}

/* Forward declarations */
static void switch_to_device(int idx);
static device_app_t* device_app_ensure(const char *name);

/* ── QR scan task: quirc only, downscale already done in camera callback ── */
static void qr_scan_task(void *arg) {
    ESP_LOGI("QR", "QR scan task started");

    struct quirc *q = quirc_new();
    if (!q) { ESP_LOGE("QR", "quirc_new failed"); vTaskDelete(NULL); return; }
    if (quirc_resize(q, QR_W, QR_H) != 0) {
        ESP_LOGE("QR", "quirc_resize failed"); quirc_destroy(q); vTaskDelete(NULL); return;
    }

    int quirc_reset_cnt = 0;

    while (qr_task_running) {
        if (xSemaphoreTake(qr_read_sem, pdMS_TO_TICKS(500)) != pdTRUE) continue;

        /* Invalidate cache: qr_gray was written by camera callback on core 0 */
        if (qr_gray && qr_gray_size > 0) {
            esp_cache_msync(qr_gray, qr_gray_size,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

            /* Reset quirc instance if it keeps failing */
            if (q && quirc_reset_cnt > 10) {
                quirc_destroy(q); q = NULL;
                quirc_reset_cnt = 0;
            }
            if (!q) {
                q = quirc_new();
                if (q && quirc_resize(q, QR_W, QR_H) != 0) { quirc_destroy(q); q = NULL; }
            }

            if (q) {
                int qw, qh;
                uint8_t *qbuf = quirc_begin(q, &qw, &qh);
                if (qbuf && qw == QR_W && qh == QR_H) {
                    memcpy(qbuf, qr_gray, QR_W * QR_H);
                    quirc_end(q);
                    quirc_reset_cnt = 0;

                    int num = quirc_count(q);
                    if (num == 0) {
                        static int no_qr = 0;
                        if (++no_qr == 1 || no_qr % 60 == 0)
                            ESP_LOGD("QR", "No QR found (%d checks)", no_qr);
                    } else {
                        ESP_LOGI("QR", "Found %d QR code(s)", num);
                        for (int i = 0; i < num; i++) {
                            struct quirc_code code;
                            struct quirc_data data;
                            quirc_extract(q, i, &code);
                            int qr_ret = quirc_decode(&code, &data);
                            if (qr_ret != 0) {
                                ESP_LOGW("QR", "Decode failed for index %d, ret=%d", i, qr_ret);
                                continue;
                            }

                            ESP_LOGI("QR_RAW", "Payload[%d]: \"%s\" (ver=%d, ecc=%d)", i, data.payload, data.version, data.ecc_level);
                            if (esp_lv_adapter_lock(portMAX_DELAY) != ESP_OK) continue;

                            const char *payload = (const char *)data.payload;
                            const char *dev_name = payload;
                            if (strncmp(payload, "ESPNOW:", 7) == 0) dev_name = payload + 7;

                            char mac[20]; mac[0] = '\0';
                            bool app_created = false;
                            if (uart_receiver_is_paired(dev_name)) {
                                uart_receiver_get_mac_by_name(dev_name, mac, sizeof(mac));
                                if (mac[0]) uart_receiver_pair_device(dev_name, mac);
                                device_app_ensure(dev_name); app_created = true;
                                if (qr_result_label) {
                                    char info[256];
                                    snprintf(info, sizeof(info),
                                             "Already Paired\nDevice: %s%s%s",
                                             dev_name, mac[0] ? "\nMAC: " : "", mac[0] ? mac : "");
                                    lv_label_set_text(qr_result_label, info);
                                    lv_obj_remove_flag(qr_result_panel, LV_OBJ_FLAG_HIDDEN);
                                    lv_obj_set_style_bg_color(qr_result_panel, lv_color_hex(0x005500), 0);
                                }
                            } else if (uart_receiver_get_mac_by_name(dev_name, mac, sizeof(mac))) {
                                uart_receiver_pair_device(dev_name, mac);
                                device_app_ensure(dev_name); app_created = true;
                                if (qr_result_label) {
                                    char info[256];
                                    snprintf(info, sizeof(info),
                                             "Pairing Success!\nDevice: %s\nMAC: %s", dev_name, mac);
                                    lv_label_set_text(qr_result_label, info);
                                    lv_obj_remove_flag(qr_result_panel, LV_OBJ_FLAG_HIDDEN);
                                    lv_obj_set_style_bg_color(qr_result_panel, lv_color_hex(0x005500), 0);
                                }
                            } else {
                                uart_receiver_pair_device(dev_name, "");
                                device_app_ensure(dev_name); app_created = true;
                                if (qr_result_label) {
                                    char info[256];
                                    snprintf(info, sizeof(info),
                                             "Pairing: %s\nWaiting for device signal...", dev_name);
                                    lv_label_set_text(qr_result_label, info);
                                    lv_obj_remove_flag(qr_result_panel, LV_OBJ_FLAG_HIDDEN);
                                    lv_obj_set_style_bg_color(qr_result_panel, lv_color_hex(0x000055), 0);
                                }
                            }
                            save_scanned_sd(dev_name);
                            if (app_created) {
                                for (int i = 0; i < MAX_DEVICE_APPS; i++) {
                                    if (s_device_apps[i].active && strcmp(s_device_apps[i].name, dev_name) == 0) {
                                        switch_to_device(i);
                                        break;
                                    }
                                }
                            }
                            esp_lv_adapter_unlock();
                        }
                    }
                } else {
                    quirc_reset_cnt++;
                }
            }
        }

        /* Signal camera callback that buffer is available for next frame */
        xSemaphoreGive(qr_write_sem);
    }

    if (q) quirc_destroy(q);
    ESP_LOGI("QR", "QR scan task exiting");
    /* Notify cleanup that we're done before deleting ourselves */
    TaskHandle_t notify = qr_cleanup_notify_task;
    qr_cleanup_notify_task = NULL;
    if (notify) xTaskNotifyGive(notify);
    vTaskDelete(NULL);
}

static void camera_stop_and_cleanup(void) {
    camera_display_initialized = false;
    camera_stream_stopping = true;
    cam_scale_applied = false;
    cam_header_set = false;
    
    if(video_fd >= 0) {
        ESP_LOGI("CAM", "Stopping video stream...");
        app_video_stream_task_stop(video_fd);
        vTaskDelay(pdMS_TO_TICKS(200));
        app_video_stream_wait_stop();
        close(video_fd);
        video_fd = -1;
        
        for(int i = 0; i < CAM_BUF_NUM; i++) {
            if(cam_buffers[i]) {
                heap_caps_free(cam_buffers[i]);
                cam_buffers[i] = NULL;
            }
        }
        
        for(int i = 0; i < DISP_BUF_COUNT; i++) {
            if(disp_buffers[i]) {
                heap_caps_free(disp_buffers[i]);
                disp_buffers[i] = NULL;
            }
        }
        
        /* Stop QR task: signal exit and wait */
        qr_task_running = false;
        if (qr_task_handle) {
            qr_cleanup_notify_task = xTaskGetCurrentTaskHandle();
            xSemaphoreGive(qr_read_sem);  /* wake the task so it sees qr_task_running=false */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            qr_task_handle = NULL;
        }
        if (qr_gray) { heap_caps_free(qr_gray); qr_gray = NULL; qr_gray_size = 0; }
        s_cam_width = 0;
        s_cam_height = 0;
        
        /* Reset video resolution override for next open */
        g_video_req_width = 0;
        g_video_req_height = 0;
        
        ESP_LOGI("CAM", "Camera and display resources freed");
    }

    camera_stream_stopping = false;
}

static void camera_cleanup_task(void *arg) {
    camera_stop_and_cleanup();
    s_camera_cleanup_pending = false;
    vTaskDelete(NULL);
}

// 助手函数：获取 UTF-8 字符的字节长度
static int get_utf8_char_len(unsigned char ch) {
    if ((ch & 0x80) == 0) return 1;
    if ((ch & 0xE0) == 0xC0) return 2;
    if ((ch & 0xF0) == 0xE0) return 3;
    if ((ch & 0xF8) == 0xF0) return 4;
    return 1;
}

// 打字机效果定时器回调
static void typewriter_timer_cb(lv_timer_t * t) {
    if (!xiaozhi_chat_label) return;

    const char * target = (const char *)lv_timer_get_user_data(t);
    if (target[s_typewriter_char_idx] == '\0') {
        lv_timer_delete(t);
        s_typewriter_timer = NULL;
        return;
    }

    int len = get_utf8_char_len((unsigned char)target[s_typewriter_char_idx]);
    int copy_len = s_typewriter_char_idx + len;
    if (copy_len >= sizeof(s_typewriter_current_buf)) {
        lv_timer_delete(t);
        s_typewriter_timer = NULL;
        return;
    }

    memcpy(s_typewriter_current_buf, target, copy_len);
    s_typewriter_current_buf[copy_len] = '\0';
    lv_label_set_text(xiaozhi_chat_label, s_typewriter_current_buf);

    s_typewriter_char_idx += len;
}

// 供主控调用的 ASR / TTS 文字更新接口
extern "C" void ui_update_xiaozhi_text(const char* text) {
    if (!text || !xiaozhi_chat_label) return;
    
    // 跨线程锁保护
    if (esp_lv_adapter_lock(pdMS_TO_TICKS(100)) == ESP_OK) {
        if (s_typewriter_timer) {
            lv_timer_delete(s_typewriter_timer);
            s_typewriter_timer = NULL;
        }

        strncpy(s_xiaozhi_target_text, text, sizeof(s_xiaozhi_target_text) - 1);
        s_xiaozhi_target_text[sizeof(s_xiaozhi_target_text) - 1] = '\0';
        
        s_typewriter_char_idx = 0;
        s_typewriter_current_buf[0] = '\0';
        
        // 开启 30ms 逐字打字机定时器
        s_typewriter_timer = lv_timer_create(typewriter_timer_cb, 30, s_xiaozhi_target_text);
        
        // Ensure island is expanded to show new text
        island_t * isl = active_island();
        if (isl && !isl->expanded) {
            toggle_island(isl);
        }
        
        esp_lv_adapter_unlock();
    }
}



static void refresh_ai_status_ui(void);

static void btn_dialogue_toggle_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bool checked = lv_obj_has_state(btn_dialogue_toggle, LV_STATE_CHECKED);
        ESP_LOGI("UI", "Dialogue toggle clicked: %d", checked);
        if (checked) {
            if (!wifi_is_connected()) {
                lv_label_set_text(siri_state_label, "WiFi Required");
                lv_obj_remove_state(btn_dialogue_toggle, LV_STATE_CHECKED);
                return;
            }
            if (xiaozhi_ai_service_start() == ESP_OK) {
                ai_service_running = true;
                ESP_LOGI("UI", "Dialogue started XiaoZhi service");
            } else {
                ESP_LOGW("UI", "Dialogue start request failed");
                lv_obj_remove_state(btn_dialogue_toggle, LV_STATE_CHECKED);
            }
        } else {
            xiaozhi_ai_service_stop();
            ai_service_running = false;
            if (ptt_pressed) {
                xiaozhi_ai_service_set_ptt(false);
                ptt_pressed = false;
            }
            ESP_LOGI("UI", "Dialogue stopped XiaoZhi service");
        }
        refresh_ai_status_ui();
    }
}

static void btn_mic_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (!ai_service_running) {
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        ESP_LOGI("UI", "Mic button pressed, starting capture");
        ptt_pressed = true;
        xiaozhi_ai_service_set_ptt(true);
        refresh_ai_status_ui();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ESP_LOGI("UI", "Mic button released, stopping capture");
        ptt_pressed = false;
        xiaozhi_ai_service_set_ptt(false);
        refresh_ai_status_ui();
    }
}

static void refresh_ai_status_ui(void) {
    island_t * isl = active_island();
    if (!isl || !isl->status_lbl) return;

    xiaozhi_state_t state = xiaozhi_ai_service_get_state();
    ai_service_running = (state != XIAOZHI_STATE_IDLE && state != XIAOZHI_STATE_ERROR);
    if (!ai_service_running) ptt_pressed = false;

    // Handle Training State
    if (g_training_progress >= 0) {
        lv_label_set_text(isl->status_lbl, "Model Training...");
        if (isl->ai_status_lbl) {
            char buf[64];
            snprintf(buf, sizeof(buf), "状态: 模型训练中 (%d%%)", g_training_progress);
            lv_label_set_text(isl->ai_status_lbl, buf);
        }
        lv_obj_set_style_bg_color(isl->led_core, lv_color_hex(0xA020F0), 0); // Purple
        lv_obj_set_style_bg_color(isl->led_glow, lv_color_hex(0xA020F0), 0);
        
        if (ai_control_panel) {
            lv_obj_add_flag(ai_control_panel, LV_OBJ_FLAG_HIDDEN);
        }
        
        // Auto expand if not expanded
        if (!isl->expanded) {
            toggle_island(isl);
        }
        
        update_island_view_mode(isl);
        return;
    }

    // Restore control panel visibility
    if (ai_control_panel) {
        lv_obj_remove_flag(ai_control_panel, LV_OBJ_FLAG_HIDDEN);
    }

    bool is_ai_mode = (ai_service_running || ptt_pressed);

    if (is_ai_mode) {
        const char * state_text = "Connecting...";
        const char * status_desc = "状态: 正在连接...";
        lv_color_t led_color = lv_color_hex(0xFFB84D); // Default yellow/orange

        switch (state) {
            case XIAOZHI_STATE_LISTENING:
                state_text = "Listening...";
                status_desc = "状态: 正在聆听...";
                led_color = lv_color_hex(0x2EE59D); // Vibrant Green
                break;
            case XIAOZHI_STATE_THINKING:
                state_text = "Thinking...";
                status_desc = "状态: 正在思考...";
                led_color = lv_color_hex(0x00F0FF); // Cyan
                break;
            case XIAOZHI_STATE_SPEAKING:
                state_text = "Speaking";
                status_desc = "状态: 正在播报...";
                led_color = lv_color_hex(0x8C52FF); // Purple/Blue
                break;
            case XIAOZHI_STATE_ACTIVATING:
                state_text = "Activating...";
                status_desc = "状态: 正在激活...";
                led_color = lv_color_hex(0xFF9F0A); // Orange
                break;
            case XIAOZHI_STATE_CONNECTED:
                state_text = "Connected";
                status_desc = "P4 可以通过文字对话让 AI 查询传感器、下发指令、分析图片、启动训练、预测保质期。";
                led_color = lv_color_hex(0x39D98A); // Green
                break;
            case XIAOZHI_STATE_ERROR:
                state_text = "Error";
                status_desc = "状态: 连接错误";
                led_color = lv_color_hex(0xFF3B30); // Red
                break;
            default:
                break;
        }

        if (ptt_pressed) {
            state_text = "Recording...";
            status_desc = "状态: 正在录音...";
            led_color = lv_color_hex(0x2EE59D);
        }

        /* Only update when content actually changes to avoid repaint flicker
         * from the 500ms timer fighting with the aurora shadow animation. */
        static char s_ai_last_text[48] = {0};
        static char s_ai_last_desc[128] = {0};
        static uint32_t s_ai_last_led = 0xFFFFFFFF;
        uint32_t cur_led = led_color.red | (led_color.green << 8) | (led_color.blue << 16);
        if (strcmp(s_ai_last_text, state_text) != 0 ||
            strcmp(s_ai_last_desc, status_desc) != 0 ||
            s_ai_last_led != cur_led) {
            lv_label_set_text(isl->status_lbl, state_text);
            if (isl->ai_status_lbl) lv_label_set_text(isl->ai_status_lbl, status_desc);
            lv_obj_set_style_bg_color(isl->led_core, led_color, 0);
            lv_obj_set_style_bg_color(isl->led_glow, led_color, 0);
            strncpy(s_ai_last_text, state_text, sizeof(s_ai_last_text) - 1);
            strncpy(s_ai_last_desc, status_desc, sizeof(s_ai_last_desc) - 1);
            s_ai_last_led = cur_led;
        }
    }

    // Auto expand/collapse transitions
    static bool s_ai_mode_last = false;
    if (is_ai_mode != s_ai_mode_last) {
        if (is_ai_mode) {
            if (!isl->expanded) toggle_island(isl);
            /* Enter AI mode: start Siri-style vibrant multicolor glow */
            start_aurora_animation(isl->capsule);
        } else {
            if (isl->expanded) toggle_island(isl);
            /* Exit AI mode: stop glow, restore plain capsule */
            stop_aurora_animation(isl->capsule);
        }
        s_ai_mode_last = is_ai_mode;
    }

    /* When idle (not in dialogue / recording / training), show backend AI
     * auto-optimization status instead of a static "System Online". */
    if (!is_ai_mode) {
        const char *ai_text   = "AI 待命中";
        const char *ai_desc   = "后台 AI 自动监控就绪";
        lv_color_t  led_color = lv_color_hex(0x39D98A); /* green - normal */

        if (g_p4_ui_status_mutex &&
            xSemaphoreTake((SemaphoreHandle_t)g_p4_ui_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (!g_p4_ui_status.cloud_online) {
                ai_text   = "云端离线";
                ai_desc   = "正在尝试重新连接云端";
                led_color = lv_color_hex(0x666666); /* grey */
            } else if (g_p4_ui_status.valid && g_p4_ui_status.ai_state_text[0]) {
                ai_text = g_p4_ui_status.ai_state_text;
                if (g_p4_ui_status.last_action[0])       ai_desc = g_p4_ui_status.last_action;
                else if (g_p4_ui_status.current_task[0]) ai_desc = g_p4_ui_status.current_task;
                /* Color by ai_state enum (per UI plan §4) */
                const char *st = g_p4_ui_status.ai_state;
                if (strstr(st, "collecting") || strstr(st, "generating") || strstr(st, "pushing"))
                    led_color = lv_color_hex(0xFFB84D);   /* yellow - executing/learning */
                else if (strstr(st, "analyzing") || strstr(st, "tuning") || strstr(st, "optimizing") ||
                         strstr(st, "validating") || strstr(st, "training"))
                    led_color = lv_color_hex(0x00F0FF);   /* cyan - analyzing/optimizing */
                else if (strstr(st, "need_confirm") || strstr(st, "offline"))
                    led_color = lv_color_hex(0xFF3B30);   /* red - needs attention */
                else
                    led_color = lv_color_hex(0x39D98A);   /* green - idle/normal */
            } else if (cloud_sync_is_enabled()) {
                ai_text   = "AI 收集数据中";
                ai_desc   = "正在汇总传感器与新鲜度数据";
                led_color = lv_color_hex(0xFFB84D);
            }
            xSemaphoreGive((SemaphoreHandle_t)g_p4_ui_status_mutex);
        }

        /* Only update labels/colors when content actually changes, to avoid
         * repaint flicker from the 500ms timer re-setting identical text. */
        static char s_last_text[48] = {0};
        static char s_last_desc[128] = {0};
        static uint32_t s_last_led_color = 0xFFFFFFFF;
        uint32_t cur_led_color = led_color.red | (led_color.green << 8) | (led_color.blue << 16);
        bool changed = (strcmp(s_last_text, ai_text) != 0) ||
                       (strcmp(s_last_desc, ai_desc) != 0) ||
                       (s_last_led_color != cur_led_color);

        if (changed) {
            lv_label_set_text(isl->status_lbl, ai_text);
            if (isl->ai_status_lbl) lv_label_set_text(isl->ai_status_lbl, ai_desc);
            lv_obj_set_style_bg_color(isl->led_core, led_color, 0);
            lv_obj_set_style_bg_color(isl->led_glow, led_color, 0);
            strncpy(s_last_text, ai_text, sizeof(s_last_text) - 1);
            strncpy(s_last_desc, ai_desc, sizeof(s_last_desc) - 1);
            s_last_led_color = cur_led_color;
        }
    }

    update_island_view_mode(isl);

    /* Update Dialogue toggle button state */
    if (btn_dialogue_toggle) {
        if (ai_service_running) {
            lv_obj_add_state(btn_dialogue_toggle, LV_STATE_CHECKED);
            if (lbl_dialogue_btn) {
                lv_label_set_text(lbl_dialogue_btn, LV_SYMBOL_CLOSE);
            }
            lv_obj_set_style_bg_color(btn_dialogue_toggle, lv_color_hex(0xFF3B30), 0); // Red
        } else {
            lv_obj_remove_state(btn_dialogue_toggle, LV_STATE_CHECKED);
            if (lbl_dialogue_btn) {
                lv_label_set_text(lbl_dialogue_btn, LV_SYMBOL_PLAY);
            }
            lv_obj_set_style_bg_color(btn_dialogue_toggle, lv_color_hex(0x007AFF), 0); // Blue
        }
    }

    /* Update Microphone toggle button state */
    if (btn_mic_toggle) {
        if (!ai_service_running) {
            lv_obj_add_state(btn_mic_toggle, LV_STATE_DISABLED);
            lv_obj_remove_state(btn_mic_toggle, LV_STATE_CHECKED);
            if (lbl_mic_btn) {
                lv_label_set_text(lbl_mic_btn, LV_SYMBOL_AUDIO);
            }
            lv_obj_set_style_bg_color(btn_mic_toggle, lv_color_hex(0x4E5D78), 0); // Grey bg
        } else {
            lv_obj_remove_state(btn_mic_toggle, LV_STATE_DISABLED);
            if (ptt_pressed) {
                lv_obj_add_state(btn_mic_toggle, LV_STATE_CHECKED);
                if (lbl_mic_btn) {
                    lv_label_set_text(lbl_mic_btn, LV_SYMBOL_MUTE);
                }
                lv_obj_set_style_bg_color(btn_mic_toggle, lv_color_hex(0x34C759), 0); // Green bg
            } else {
                lv_obj_remove_state(btn_mic_toggle, LV_STATE_CHECKED);
                if (lbl_mic_btn) {
                    lv_label_set_text(lbl_mic_btn, LV_SYMBOL_AUDIO);
                }
                lv_obj_set_style_bg_color(btn_mic_toggle, lv_color_hex(0x1C1C1E), 0); // Dark grey bg
            }
        }
    }


}

static void refresh_apps_drawer_widgets(void) {
    if (!apps_panel || !apps_drawer_open) return;

    // 1. Update Heap Memory Arc (show used % of total PSRAM heap)
    if (side_mem_arc && side_mem_lbl) {
        size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t free_h = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        int pct = 0;
        if (total > 0) {
            size_t used = (free_h <= total) ? (total - free_h) : 0;
            pct = (int)(used * 100 / total);
            if (pct > 100) pct = 100;
            if (pct < 0) pct = 0;
        }
        lv_arc_set_value(side_mem_arc, pct);

        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(side_mem_lbl, buf);
    }

    // 2. Update Temp/Humi Labels
    extern float g_temperature;
    extern float g_humidity;
    
    if (side_temp_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "温度: %.1f°C", g_temperature);
        lv_label_set_text(side_temp_lbl, buf);
    }
    if (side_humi_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "湿度: %.1f%%", g_humidity);
        lv_label_set_text(side_humi_lbl, buf);
    }
}

static void fake_data_timer_cb(lv_timer_t * timer) {
    static int hb_count = 0;
    if (++hb_count % 20 == 1) ESP_LOGI("UI_TMR", "fake_data_timer_cb alive (hb=%d)", hb_count);
    
    // Print memory usage in real-time every 2 seconds (4 * 500ms)
    if (hb_count % 4 == 0) {
        size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t min_int = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t min_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI("MEM", "Internal RAM: Free=%d KB (Min=%d KB), PSRAM: Free=%d KB (Min=%d KB)",
                 (int)(free_int / 1024), (int)(min_int / 1024),
                 (int)(free_psram / 1024), (int)(min_psram / 1024));
    }
    
    refresh_ai_status_ui();
    refresh_apps_drawer_widgets();

    // Auto-stop temporary TTS service if active and playback is complete or timed out
    if (g_temp_tts_active && ai_service_running) {
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() / 1000) - g_temp_tts_start_time_ms;
        extern xiaozhi_state_t xiaozhi_ai_service_get_state(void);
        extern esp_err_t xiaozhi_ai_service_stop(void);
        xiaozhi_state_t state = xiaozhi_ai_service_get_state();
        if (elapsed > 15000 || ((state == XIAOZHI_STATE_CONNECTED || state == XIAOZHI_STATE_IDLE || state == XIAOZHI_STATE_ERROR) && elapsed > 6000)) {
            ESP_LOGI("UI", "Auto-stopping temporary TTS service after %u ms (state=%d)", (unsigned)elapsed, (int)state);
            xiaozhi_ai_service_stop();
            ai_service_running = false;
            g_temp_tts_active = false;
            refresh_ai_status_ui();
        }
    }
    // WiFi status UI update (runs in LVGL context - safe)
    {
        static int last_wifi_ui_status = -1;
        static uint32_t s_last_reconnect_tick = 0;
        int status = wifi_conn_status;
        if (status != last_wifi_ui_status) {
            last_wifi_ui_status = status;
            wifi_status_timer_ticks = 0;
        }
        if (status != 3) {
            s_last_reconnect_tick = 0;
        }
        if (status == 2) {
            char status_text[128];
            snprintf(status_text, sizeof(status_text), "WiFi: %s", selected_ssid);
            set_wifi_status_text(status_text, lv_color_hex(0x00FF00));
        } else if (status == 1) {
            wifi_status_timer_ticks = wifi_status_timer_ticks + 1;
            if (wifi_status_timer_ticks > 20) {
                // Over 10 seconds (20 * 500ms): timeout
                ESP_LOGW("WIFI", "WiFi connect timeout (ticks=%d)", (int)wifi_status_timer_ticks);
                set_wifi_status_text("WiFi: Timeout", lv_color_hex(0xFF6600));
                wifi_conn_status = 3;
                last_wifi_ui_status = 3;
            } else {
                set_wifi_status_text("WiFi: Connecting...", lv_color_hex(0xFFA500));
            }
        } else if (status == 3) {
            set_wifi_status_text("WiFi: Disconnected", lv_color_hex(0xFF0000));
            
            // 自动退避重连逻辑：在断开连接状态下，如果 NVS 中有保存的配置，每 30 秒尝试一次自动重连
            uint32_t now = xTaskGetTickCount();
            if (s_last_reconnect_tick == 0) {
                s_last_reconnect_tick = now;
            }
            if (now - s_last_reconnect_tick > pdMS_TO_TICKS(30000)) {
                s_last_reconnect_tick = now;
                char saved_ssid[33] = {}, saved_pass[65] = {};
                if (load_wifi_creds(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass))) {
                    ESP_LOGI("WIFI", "Timer auto-reconnect: trying saved AP: %s", saved_ssid);
                    wifi_conn_status = 1;
                    wifi_status_timer_ticks = 0;
                    esp_wifi_connect();
                }
            }
        }
    }

    if (c6_status_label && c6_status_dot) {
        bool c6_ok = uart_receiver_is_connected();
        lv_obj_set_style_bg_color(c6_status_dot, c6_ok ? lv_color_hex(0x39D98A) : lv_color_hex(0x666666), 0);
        lv_label_set_text(c6_status_label, c6_ok ? "Connected" : "Disconnected");
        lv_obj_set_style_text_color(c6_status_label, c6_ok ? lv_color_hex(0x39D98A) : lv_color_hex(0x91A0B8), 0);
        // 动态 C6 桥接卡片高亮边框
        if (c6_tile) {
            lv_obj_set_style_border_color(c6_tile, c6_ok ? lv_color_hex(0x03DAC6) : lv_color_hex(0x2A3A5A), 0);
            lv_obj_set_style_border_width(c6_tile, c6_ok ? 2 : 1, 0);
        }
    }

    if (s_udp_data_mutex != NULL && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        /* Update warmup label — real-time countdown (C6 only pushes every ~10s) */
        if (s_warmup_lbl) {
            static int s_display_warmup = -1;
            static uint32_t s_warmup_ticks = 0;
            int rem = uart_receiver_get_warmup_remaining();
            if (rem > 0) {
                if (rem != s_display_warmup) {
                    s_display_warmup = rem;
                    s_warmup_ticks = 0;
                } else {
                    s_warmup_ticks++;
                    if (s_warmup_ticks >= 2) {
                        s_warmup_ticks = 0;
                        if (s_display_warmup > 0) s_display_warmup--;
                    }
                }
                if (s_display_warmup < 0) s_display_warmup = 0;
                lv_label_set_text_fmt(s_warmup_lbl, "Warmup: %d s", s_display_warmup);
                lv_obj_remove_flag(s_warmup_lbl, LV_OBJ_FLAG_HIDDEN);
            } else {
                s_display_warmup = -1;
                s_warmup_ticks = 0;
                lv_obj_add_flag(s_warmup_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_uv_remain_lbl) {
            lv_label_set_text_fmt(s_uv_remain_lbl, "UV remain: %d", s_udp_node1.uv_remain);
        }
        if (s_uv_dur_lbl) {
            /* Sync local setting from UART data if it changed */
            if (s_udp_node1.uv_dur > 0 && s_udp_node1.uv_dur != s_uv_dur_setting) {
                s_uv_dur_setting = s_udp_node1.uv_dur;
            }
            lv_label_set_text_fmt(s_uv_dur_lbl, "UV dur: %ds", s_uv_dur_setting);
        }

        /* ── Shared device detail screen (update from current device) ── */
        if (s_current_device_idx >= 0 && s_current_device_idx < MAX_DEVICE_APPS) {
            device_app_t *a = &s_device_apps[s_current_device_idx];
            if (a->active && d_voc_label) {
                if (a->data.valid) {
                    int voc_i = (int)(a->data.voc * 10);
                    int odor_i = (int)(a->data.odor * 100);
                    int hcho_i = (int)(a->data.hcho * 100);
                    int co_i = (int)(a->data.co * 100);
                    int conf_i = (int)(a->data.conf * 100);
                    lv_label_set_text_fmt(d_voc_label, "VOC: %d.%d ppm", voc_i / 10, voc_i % 10);
                    lv_label_set_text_fmt(d_co2_label, "CO2: %d ppm", a->data.co2);
                    lv_label_set_text_fmt(d_odor_label, "Odor: %d.%02d ppm", odor_i / 100, odor_i % 100);
                    lv_label_set_text_fmt(d_hcho_label, "HCHO: %d.%02d ppm", hcho_i / 100, hcho_i % 100);
                    lv_label_set_text_fmt(d_co_label, "CO: %d.%02d ppm", co_i / 100, co_i % 100);
                    lv_label_set_text_fmt(d_pred_label, "Pred: %d (%s, %d%%)",
                        a->data.pred, a->data.sensor_class, conf_i);
                    if (s_detail_chart && s_chart_ser_voc) {
                        lv_chart_set_next_value(s_detail_chart, s_chart_ser_voc, voc_i);
                    }
                } else {
                    lv_label_set_text(d_voc_label, "VOC: --");
                    lv_label_set_text(d_co2_label, "CO2: --");
                    lv_label_set_text(d_odor_label, "Odor: --");
                    lv_label_set_text(d_hcho_label, "HCHO: --");
                    lv_label_set_text(d_co_label, "CO: --");
                    lv_label_set_text(d_pred_label, "Waiting for data...");
                }
            }
        }
        xSemaphoreGive(s_udp_data_mutex);
    }

    /* Update model info labels and status LED */
    ModelMgrStateInfo active_state;
    model_mgr_get_state(&active_state);

    if (main_model_name_label) {
        if (g_training_progress >= 0) {
            lv_label_set_text_fmt(main_model_name_label, "Model: Training %d%%...", g_training_progress);
        } else if (active_state.state != MCS_IDLE && strlen(active_state.name) > 0) {
            if (active_state.state == MCS_DOWNLOADING) {
                lv_label_set_text_fmt(main_model_name_label, "Model: %s [Cloud DL...]", active_state.name);
            } else {
                lv_label_set_text_fmt(main_model_name_label, "Model: %s [Syncing %d%%]", active_state.name, active_state.progress_percent);
            }
        } else {
            lv_label_set_text_fmt(main_model_name_label, "Model: %s (v%s)", g_receiver_model_name, g_receiver_model_version);
        }
    }
    if (main_model_classes_label) {
        if (g_training_progress >= 0) {
            lv_label_set_text(main_model_classes_label, "Classes: -- (Training...)");
        } else if (active_state.state != MCS_IDLE && strlen(active_state.name) > 0) {
            lv_label_set_text(main_model_classes_label, "Classes: -- (Transferring...)");
        } else {
            lv_label_set_text_fmt(main_model_classes_label, "Classes: %s", g_receiver_model_classes);
        }
    }
    if (main_model_status_led) {
        if (g_training_progress >= 0) {
            lv_led_on(main_model_status_led);
            lv_obj_set_style_bg_color(main_model_status_led, lv_color_hex(0x63B3ED), 0); // Tech blue during training
        } else if (active_state.state != MCS_IDLE) {
            lv_led_on(main_model_status_led);
            lv_obj_set_style_bg_color(main_model_status_led, lv_color_hex(0xFFB84D), 0); // Orange during sync/download
        } else {
            lv_obj_set_style_bg_color(main_model_status_led, lv_color_hex(0x39D98A), 0); // Ready green
            if (g_receiver_model_ready) {
                lv_led_on(main_model_status_led);
            } else {
                lv_led_off(main_model_status_led);
            }
        }
    }

    if (main_model_bar) {
        if (g_training_progress >= 0) {
            lv_obj_clear_flag(main_model_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(main_model_bar, g_training_progress, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(main_model_bar, lv_color_hex(0x63B3ED), LV_PART_INDICATOR);
        } else if (active_state.state != MCS_IDLE && strlen(active_state.name) > 0) {
            lv_obj_clear_flag(main_model_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(main_model_bar, active_state.progress_percent, LV_ANIM_OFF);
            if (active_state.state == MCS_DOWNLOADING) {
                lv_obj_set_style_bg_color(main_model_bar, lv_color_hex(0xFFB84D), LV_PART_INDICATOR);
            } else {
                lv_obj_set_style_bg_color(main_model_bar, lv_color_hex(0x39D98A), LV_PART_INDICATOR);
            }
        } else {
            lv_obj_add_flag(main_model_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Update modal progress UI if train_modal is visible */
    {
        static bool s_completion_shown = false;
        train_info_t info = {0};
        bool active = false;
        if (g_train_info_mutex && xSemaphoreTake((QueueHandle_t)g_train_info_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            info = g_train_info;
            active = info.active;
            xSemaphoreGive((QueueHandle_t)g_train_info_mutex);
        }

        if (train_modal && !lv_obj_has_flag(train_modal, LV_OBJ_FLAG_HIDDEN)) {
            if (active) {
                // Hide selection / settings widgets
                if (train_list_title) lv_obj_add_flag(train_list_title, LV_OBJ_FLAG_HIDDEN);
                if (train_csv_list) lv_obj_add_flag(train_csv_list, LV_OBJ_FLAG_HIDDEN);
                if (train_name_lbl) lv_obj_add_flag(train_name_lbl, LV_OBJ_FLAG_HIDDEN);
                if (train_model_name_ta) lv_obj_add_flag(train_model_name_ta, LV_OBJ_FLAG_HIDDEN);
                if (train_acc_lbl) lv_obj_add_flag(train_acc_lbl, LV_OBJ_FLAG_HIDDEN);
                if (train_accuracy_label) lv_obj_add_flag(train_accuracy_label, LV_OBJ_FLAG_HIDDEN);
                if (train_accuracy_slider) lv_obj_add_flag(train_accuracy_slider, LV_OBJ_FLAG_HIDDEN);
                if (btn_start_train) lv_obj_add_flag(btn_start_train, LV_OBJ_FLAG_HIDDEN);

                // Show progress container
                if (train_progress_cont) lv_obj_clear_flag(train_progress_cont, LV_OBJ_FLAG_HIDDEN);

                // Update widgets
                if (modal_model_name_val) {
                    lv_label_set_text_fmt(modal_model_name_val, "Model Name: %s", info.model_name);
                }
                if (modal_status_badge) {
                    const char *status_str = "IDLE";
                    uint32_t bg_color = 0x4A5568; // Grey
                    switch (info.status) {
                        case TRAIN_STATUS_QUEUED:
                            status_str = "Queued";
                            bg_color = 0xD69E2E; // Dark Yellow
                            break;
                        case TRAIN_STATUS_TRAINING:
                            status_str = "Training";
                            bg_color = 0x3182CE; // Blue
                            break;
                        case TRAIN_STATUS_SIMULATING:
                            status_str = "Simulating";
                            bg_color = 0x805AD5; // Purple
                            break;
                        case TRAIN_STATUS_COMPLETED:
                            status_str = "Completed";
                            bg_color = 0x38A169; // Green
                            break;
                        case TRAIN_STATUS_FAILED:
                            status_str = "Failed";
                            bg_color = 0xE53E3E; // Red
                            break;
                        case TRAIN_STATUS_CANCELLED:
                            status_str = "Cancelled";
                            bg_color = 0x718096; // Grey
                            break;
                        case TRAIN_STATUS_TIMEOUT:
                            status_str = "Timeout";
                            bg_color = 0xDD6B20; // Red-orange
                            break;
                        default:
                            break;
                    }
                    lv_label_set_text(modal_status_badge, status_str);
                    lv_obj_set_style_bg_color(modal_status_badge, lv_color_hex(bg_color), 0);
                }
                if (modal_phase_val) {
                    lv_label_set_text_fmt(modal_phase_val, "Phase: %s", info.phase);
                }
                if (modal_training_bar) {
                    lv_bar_set_value(modal_training_bar, info.progress, LV_ANIM_OFF);
                }
                if (modal_training_label) {
                    lv_label_set_text_fmt(modal_training_label, "%d%%", info.progress);
                }
                if (modal_epoch_val) {
                    lv_label_set_text_fmt(modal_epoch_val, "Epoch: %d / %d", info.epoch, info.total_epochs);
                }
                if (modal_accuracy_val) {
                    lv_label_set_text_fmt(modal_accuracy_val, "Accuracy: %.4f", info.accuracy);
                }
                if (modal_loss_val) {
                    lv_label_set_text_fmt(modal_loss_val, "Loss: %.4f", info.loss);
                }
                if (modal_training_status) {
                    if (info.status == TRAIN_STATUS_FAILED || info.status == TRAIN_STATUS_TIMEOUT) {
                        lv_label_set_text_fmt(modal_training_status, "Error: %s", info.error);
                    } else {
                        lv_label_set_text(modal_training_status, "");
                    }
                }

                // Completion dialog logic
                if (info.status == TRAIN_STATUS_QUEUED || info.status == TRAIN_STATUS_TRAINING || info.status == TRAIN_STATUS_SIMULATING) {
                    s_completion_shown = false;
                }
                if (!s_completion_shown) {
                    if (info.status == TRAIN_STATUS_COMPLETED) {
                        s_completion_shown = true;
                        char msg_buf[256];
                        snprintf(msg_buf, sizeof(msg_buf), "Model Name: %s\nEpochs: %d\nAccuracy: %.2f%%\n\nDownload starts automatically...", 
                                 info.model_name, info.epoch, info.accuracy * 100.0f);
                        lv_obj_t * mbox = lv_msgbox_create(NULL);
                        
                        lv_obj_t * title_lbl = lv_msgbox_add_title(mbox, "Training Success");
                        if (title_lbl) {
                            lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x38A169), 0); // Green title
                            lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
                        }
                        
                        lv_obj_t * text_lbl = lv_msgbox_add_text(mbox, msg_buf);
                        if (text_lbl) {
                            lv_obj_set_style_text_color(text_lbl, lv_color_hex(0xFFFFFF), 0); // White text
                            lv_obj_set_style_text_font(text_lbl, &lv_font_montserrat_14, 0);
                        }
                        
                        lv_obj_t * close_btn = lv_msgbox_add_close_button(mbox);
                        if (close_btn) {
                            lv_obj_add_event_cb(close_btn, [](lv_event_t *ev) {
                                cloud_sync_clear_training_state();
                            }, LV_EVENT_CLICKED, NULL);
                        }
                        
                        lv_obj_t * btn = lv_msgbox_add_footer_button(mbox, "OK");
                        if (btn) {
                            lv_obj_set_style_bg_color(btn, lv_color_hex(0x38A169), 0); // Green button
                            lv_obj_set_style_text_color(btn, lv_color_hex(0x131828), 0); // Dark text on green background
                            lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
                        }
                        
                        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
                            lv_obj_t *target = (lv_obj_t *)lv_event_get_target(ev);
                            lv_obj_t *f = lv_obj_get_parent(target);
                            if (f) {
                                lv_obj_t *mb = lv_obj_get_parent(f);
                                if (mb) lv_msgbox_close(mb);
                            }
                            cloud_sync_clear_training_state();
                        }, LV_EVENT_CLICKED, NULL);
                        
                        lv_obj_center(mbox);
                        lv_obj_set_style_bg_color(mbox, lv_color_hex(0x1B2238), 0);
                        lv_obj_set_style_border_color(mbox, lv_color_hex(0x38A169), 0);
                        lv_obj_set_style_border_width(mbox, 2, 0);
                    } else if (info.status == TRAIN_STATUS_FAILED || info.status == TRAIN_STATUS_TIMEOUT) {
                        s_completion_shown = true;
                        char msg_buf[256];
                        snprintf(msg_buf, sizeof(msg_buf), "Reason:\n%s", strlen(info.error) > 0 ? info.error : "Unknown error");
                        lv_obj_t * mbox = lv_msgbox_create(NULL);
                        
                        lv_obj_t * title_lbl = lv_msgbox_add_title(mbox, "Training Failed");
                        if (title_lbl) {
                            lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xE53E3E), 0); // Red title
                            lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
                        }
                        
                        lv_obj_t * text_lbl = lv_msgbox_add_text(mbox, msg_buf);
                        if (text_lbl) {
                            lv_obj_set_style_text_color(text_lbl, lv_color_hex(0xFFFFFF), 0); // White text
                            lv_obj_set_style_text_font(text_lbl, &lv_font_montserrat_14, 0);
                        }
                        
                        lv_obj_t * close_btn = lv_msgbox_add_close_button(mbox);
                        if (close_btn) {
                            lv_obj_add_event_cb(close_btn, [](lv_event_t *ev) {
                                cloud_sync_clear_training_state();
                            }, LV_EVENT_CLICKED, NULL);
                        }
                        
                        lv_obj_t * btn = lv_msgbox_add_footer_button(mbox, "Dismiss");
                        if (btn) {
                            lv_obj_set_style_bg_color(btn, lv_color_hex(0xE53E3E), 0); // Red button
                            lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0); // White text on red background
                            lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
                        }
                        
                        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
                            lv_obj_t *target = (lv_obj_t *)lv_event_get_target(ev);
                            lv_obj_t *f = lv_obj_get_parent(target);
                            if (f) {
                                lv_obj_t *mb = lv_obj_get_parent(f);
                                if (mb) lv_msgbox_close(mb);
                            }
                            cloud_sync_clear_training_state();
                        }, LV_EVENT_CLICKED, NULL);
                        
                        lv_obj_center(mbox);
                        lv_obj_set_style_bg_color(mbox, lv_color_hex(0x1B2238), 0);
                        lv_obj_set_style_border_color(mbox, lv_color_hex(0xE53E3E), 0);
                        lv_obj_set_style_border_width(mbox, 2, 0);
                    }
                }
            } else {
                // Restore selection / settings widgets
                if (train_list_title) lv_obj_remove_flag(train_list_title, LV_OBJ_FLAG_HIDDEN);
                if (train_csv_list) lv_obj_remove_flag(train_csv_list, LV_OBJ_FLAG_HIDDEN);
                if (train_name_lbl) lv_obj_remove_flag(train_name_lbl, LV_OBJ_FLAG_HIDDEN);
                if (train_model_name_ta) lv_obj_remove_flag(train_model_name_ta, LV_OBJ_FLAG_HIDDEN);
                if (train_acc_lbl) lv_obj_remove_flag(train_acc_lbl, LV_OBJ_FLAG_HIDDEN);
                if (train_accuracy_label) lv_obj_remove_flag(train_accuracy_label, LV_OBJ_FLAG_HIDDEN);
                if (train_accuracy_slider) lv_obj_remove_flag(train_accuracy_slider, LV_OBJ_FLAG_HIDDEN);
                if (btn_start_train) lv_obj_remove_flag(btn_start_train, LV_OBJ_FLAG_HIDDEN);

                // Hide progress container
                if (train_progress_cont) lv_obj_add_flag(train_progress_cont, LV_OBJ_FLAG_HIDDEN);
                s_completion_shown = false;
            }
        }
    }
}

/* 
 * 提示：以前的 parse_udp_sensor_packet 和 udp_listener_task 已被彻底删除。
 * 传感器数据现已全面统一为通过串口（UART1）进行接收和处理。
 */

static void vol_slider_event_cb(lv_event_t * e) {
    lv_obj_t * slider = (lv_obj_t *)lv_event_get_target(e);
    int vol = (int)lv_slider_get_value(slider);
    bsp_extra_codec_volume_set(vol, NULL);
    update_volume_ui(vol, false);
}

static void create_siri_orb(void) {
    if (ai_control_panel) {
        ESP_LOGD("SIRI", "Dialogue panel already created, skipping");
        return;
    }
    ESP_LOGI("SIRI", "Creating Dialogue & Microphone control panel...");

    /* Create Dialogue & Microphone control panel directly at bottom-left */
    ai_control_panel = lv_obj_create(scr_main);
    apply_no_shadow(ai_control_panel);
    lv_obj_set_size(ai_control_panel, 140, 70);
    lv_obj_align(ai_control_panel, LV_ALIGN_BOTTOM_LEFT, 30, -30);
    lv_obj_set_style_bg_color(ai_control_panel, lv_color_hex(0x131828), 0);
    lv_obj_set_style_bg_opa(ai_control_panel, LV_OPA_80, 0);
    lv_obj_set_style_border_color(ai_control_panel, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(ai_control_panel, 1, 0);
    lv_obj_set_style_radius(ai_control_panel, 35, 0);
    lv_obj_set_style_pad_all(ai_control_panel, 0, 0);
    lv_obj_clear_flag(ai_control_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Dialogue toggle button */
    btn_dialogue_toggle = lv_btn_create(ai_control_panel);
    lv_obj_set_size(btn_dialogue_toggle, 48, 48);
    lv_obj_align(btn_dialogue_toggle, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_flag(btn_dialogue_toggle, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_radius(btn_dialogue_toggle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn_dialogue_toggle, 1, 0);
    lv_obj_set_style_border_color(btn_dialogue_toggle, lv_color_hex(0x2A3A5A), 0);
    lv_obj_add_event_cb(btn_dialogue_toggle, btn_dialogue_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lbl_dialogue_btn = lv_label_create(btn_dialogue_toggle);
    lv_label_set_text(lbl_dialogue_btn, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(lbl_dialogue_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_dialogue_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_dialogue_btn);

    /* Microphone Push-to-Talk button */
    btn_mic_toggle = lv_btn_create(ai_control_panel);
    lv_obj_set_size(btn_mic_toggle, 48, 48);
    lv_obj_align(btn_mic_toggle, LV_ALIGN_RIGHT_MID, -12, 0);
    // Not checkable - functions as a momentary push button
    lv_obj_clear_flag(btn_mic_toggle, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_radius(btn_mic_toggle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn_mic_toggle, 1, 0);
    lv_obj_set_style_border_color(btn_mic_toggle, lv_color_hex(0x2A3A5A), 0);
    // Bind pressed, released, and press lost events to the momentary event handler
    lv_obj_add_event_cb(btn_mic_toggle, btn_mic_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn_mic_toggle, btn_mic_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(btn_mic_toggle, btn_mic_event_cb, LV_EVENT_PRESS_LOST, NULL);

    lbl_mic_btn = lv_label_create(btn_mic_toggle);
    lv_label_set_text(lbl_mic_btn, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(lbl_mic_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_mic_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_mic_btn);

    refresh_ai_status_ui();
}

static void btn_enter_event_handler(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI("UI", "Enter dashboard");
        lv_screen_load(scr_main);
        if (s_udp_data_mutex == NULL) {
            s_udp_data_mutex = xSemaphoreCreateMutex();
        }
    }
}

static void ta_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = (lv_obj_t*)lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(kb);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_connect_btn_cb(lv_event_t * e) {
    LV_UNUSED(e);
    lv_obj_add_flag(wifi_pass_modal, LV_OBJ_FLAG_HIDDEN);
    set_wifi_status_text("WiFi: Connecting...", lv_color_hex(0xFFA500));

    const char * pass = lv_textarea_get_text(ta_pass);

    esp_wifi_scan_stop();
    wifi_config_t wifi_config = {};
    snprintf((char*)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", selected_ssid);
    snprintf((char*)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", pass);

    ESP_LOGI("WIFI", "Connecting to %s...", selected_ssid);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    wifi_conn_status = 1;
    wifi_status_timer_ticks = 0;
    wifi_disconnect_count = 0;
}

static void wifi_pass_back_btn_cb(lv_event_t * e) {
    LV_UNUSED(e);
    lv_obj_add_flag(wifi_pass_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifi_list_modal);
    lv_obj_remove_flag(wifi_list_modal, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_list_close_btn_cb(lv_event_t * e) {
    lv_obj_add_flag(wifi_list_modal, LV_OBJ_FLAG_HIDDEN);
    // 如果扫描时断开了连接且用户没手动连接,关闭对话框时重连
    if (wifi_conn_status != 2) {
        wifi_config_t conf;
        if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0] != '\0') {
            ESP_LOGI("WIFI", "Reconnecting after scan dialog closed to %s", conf.sta.ssid);
            wifi_conn_status = 1;
            wifi_status_timer_ticks = 0;
            esp_wifi_connect();
        }
    }
}

static void wifi_list_btn_cb(lv_event_t * e) {
    lv_obj_t * list_btn = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(list_btn, 0);
    const char * ssid = label ? lv_label_get_text(label) : NULL;
    if(ssid) {
        snprintf(selected_ssid, sizeof(selected_ssid), "%s", ssid);
        char title_text[128];
        snprintf(title_text, sizeof(title_text), "连接到: %s", ssid);
        lv_label_set_text(wifi_pass_title, title_text);
        lv_textarea_set_text(ta_pass, "");
        lv_obj_add_flag(wifi_list_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wifi_pass_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

#define NVS_WIFI_NAMESPACE "wifi_creds"

static void save_wifi_creds(const char *ssid, const char *pass) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW("WIFI", "NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err == ESP_OK) {
        ESP_LOGI("WIFI", "Saved WiFi creds to NVS (SSID: %s)", ssid);
    } else {
        ESP_LOGW("WIFI", "NVS save failed: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

static bool load_wifi_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }
    size_t len = ssid_len;
    err = nvs_get_str(handle, "ssid", ssid, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    len = pass_len;
    err = nvs_get_str(handle, "pass", pass, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    ESP_LOGI("WIFI", "Loaded WiFi creds from NVS (SSID: %s)", ssid);
    return true;
}

#define NVS_UI_NAMESPACE "ui_config"

static void save_wallpaper_path(const char *path) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_UI_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return;
    nvs_set_str(handle, "wallpaper", path);
    nvs_commit(handle);
    nvs_close(handle);
}

static bool load_wallpaper_path(char *path, size_t len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_UI_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    size_t plen = len;
    err = nvs_get_str(handle, "wallpaper", path, &plen);
    nvs_close(handle);
    if (err == ESP_OK && path[0] != '\0') {
        ESP_LOGI("UI", "Loaded wallpaper path: %s", path);
        return true;
    }
    return false;
}

static void save_startup_wallpaper_path(const char *path) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_UI_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return;
    nvs_set_str(handle, "startup_wp", path);
    nvs_commit(handle);
    nvs_close(handle);
}

static bool load_startup_wallpaper_path(char *path, size_t len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_UI_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    size_t plen = len;
    err = nvs_get_str(handle, "startup_wp", path, &plen);
    nvs_close(handle);
    if (err == ESP_OK && path[0] != '\0') {
        ESP_LOGI("UI", "Loaded startup wallpaper path: %s", path);
        return true;
    }
    return false;
}

static bool wallpaper_ends_with(const char *path, const char *ext) {
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) return false;
    const char *suffix = path + plen - elen;
    while (*suffix && *ext) {
        char c1 = *suffix++;
        char c2 = *ext++;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return false;
    }
    return true;
}

static void set_wallpaper(const char *file_path) {
    if (!file_path || !file_path[0]) return;

    int slot = (s_wallpaper_target == WALLPAPER_TARGET_STARTUP) ? 0 : 1;
    const lv_image_dsc_t *img = sd_card_bg_load_jpeg_slot(file_path, slot);
    if (!img) {
        ESP_LOGW("UI", "Failed to load wallpaper: %s", file_path);
        return;
    }

    if (s_wallpaper_target == WALLPAPER_TARGET_STARTUP) {
        if (s_startup_wallpaper_img) {
            lv_image_set_src(s_startup_wallpaper_img, img);
        }
        strncpy(s_current_startup_wallpaper_path, file_path, sizeof(s_current_startup_wallpaper_path) - 1);
        save_startup_wallpaper_path(file_path);
        ESP_LOGI("UI", "Startup wallpaper changed to: %s", file_path);
    } else {
        if (s_wallpaper_img) {
            lv_image_set_src(s_wallpaper_img, img);
        }
        strncpy(s_current_wallpaper_path, file_path, sizeof(s_current_wallpaper_path) - 1);
        save_wallpaper_path(file_path);
        ESP_LOGI("UI", "Main wallpaper changed to: %s", file_path);
    }
}

bool wifi_is_connected(void)
{
    return wifi_conn_status == 2;
}

/* ===== Startup Status Bar LED Animations & Cyclical Callback ===== */

static void breath_anim_opa_cb(void * var, int32_t v) {
    lv_obj_t * obj = (lv_obj_t *)var;
    if (obj) {
        lv_obj_set_style_bg_opa(obj, v, 0);
    }
}

typedef enum {
    SYS_DEV_SYSTEM,
    SYS_DEV_SDCARD,
    SYS_DEV_WIFI,
    SYS_DEV_SENSOR,
    SYS_DEV_BLE,
    SYS_DEV_COUNT
} sys_dev_t;

static void startup_check_timer_cb(lv_timer_t * timer) {
    LV_UNUSED(timer);
    island_t * isl = active_island();
    if (!isl || !isl->capsule || !isl->status_lbl || !isl->led_core || !isl->led_glow) return;

    bool is_ai_mode = (ai_service_running || g_training_progress >= 0 || ptt_pressed);
    if (is_ai_mode) {
        if (isl->expanded) {
            update_island_details(isl);
        }
        return;
    }

    s_startup_check_index = (s_startup_check_index + 1) % SYS_DEV_COUNT;
    
    lv_color_t color = lv_color_hex(0x39D98A);
    char text_buf[64] = "";

    switch (s_startup_check_index) {
        case SYS_DEV_SYSTEM: {
            uint32_t free_heap = esp_get_free_heap_size() / 1024;
            snprintf(text_buf, sizeof(text_buf), "System: OK (Heap %u KB)", (unsigned int)free_heap);
            color = lv_color_hex(0x39D98A);
            break;
        }
        case SYS_DEV_SDCARD: {
            bool ready = sd_card_bg_is_ready();
            snprintf(text_buf, sizeof(text_buf), "SD Card: %s", ready ? "Ready" : "Not Ready");
            color = ready ? lv_color_hex(0x39D98A) : lv_color_hex(0xFFB84D);
            break;
        }
        case SYS_DEV_WIFI: {
            bool connected = wifi_is_connected();
            if (connected) {
                snprintf(text_buf, sizeof(text_buf), "Wi-Fi: Connected");
                color = lv_color_hex(0x39D98A);
            } else if (wifi_conn_status == 1) {
                snprintf(text_buf, sizeof(text_buf), "Wi-Fi: Connecting...");
                color = lv_color_hex(0x4AA3FF);
            } else {
                snprintf(text_buf, sizeof(text_buf), "Wi-Fi: Disconnected");
                color = lv_color_hex(0xFFB84D);
            }
            break;
        }
        case SYS_DEV_SENSOR: {
            bool online = s_udp_node1.valid;
            snprintf(text_buf, sizeof(text_buf), "Sensor Link: %s", online ? "Online" : "Offline");
            color = online ? lv_color_hex(0x39D98A) : lv_color_hex(0xFFB84D);
            break;
        }
        case SYS_DEV_BLE: {
            snprintf(text_buf, sizeof(text_buf), "Bluetooth: Ready");
            color = lv_color_hex(0x4AA3FF);
            break;
        }
        default:
            break;
    }

    lv_label_set_text(isl->status_lbl, text_buf);
    lv_obj_set_style_bg_color(isl->led_core, color, 0);
    lv_obj_set_style_bg_color(isl->led_glow, color, 0);

    if (isl->expanded) {
        update_island_details(isl);
    }
}

/* ===== Island Struct & Core Functions ===== */

/* Siri-style aurora glow: lightweight layered halo (no shadow blur).
 * Uses 2 semi-transparent rounded rects behind the capsule instead of
 * lv_obj shadow — shadow blur is extremely expensive on ESP32-P4 software
 * rendering and causes frame drops. Pure color-fill layers are ~10x cheaper. */
static lv_obj_t * s_aurora_outer = NULL; /* outer glow ring */
static lv_obj_t * s_aurora_inner = NULL; /* inner glow ring */

static void aurora_color_cb(void * var, int32_t v)
{
    (void)var;
    lv_color_t c = lv_color_hsv_to_rgb(v % 360, 70, 100);
    if (s_aurora_outer) lv_obj_set_style_bg_color(s_aurora_outer, c, 0);
    if (s_aurora_inner) lv_obj_set_style_bg_color(s_aurora_inner, c, 0);
}

static void aurora_breath_cb(void * var, int32_t v)
{
    (void)var;
    /* v 0..100. Gentle opacity 15..45 outer, 25..55 inner */
    if (s_aurora_outer) lv_obj_set_style_bg_opa(s_aurora_outer, 15 + v * 30 / 100, 0);
    if (s_aurora_inner) lv_obj_set_style_bg_opa(s_aurora_inner, 25 + v * 30 / 100, 0);
}

static void start_aurora_animation(lv_obj_t * capsule)
{
    if (!capsule) return;
    lv_obj_t * parent = lv_obj_get_parent(capsule);
    if (!parent) return;

    /* Create glow layers if not yet created */
    if (!s_aurora_outer) {
        s_aurora_outer = lv_obj_create(parent);
        apply_no_shadow(s_aurora_outer);
        lv_obj_set_size(s_aurora_outer, 280, 76);
        lv_obj_align_to(s_aurora_outer, capsule, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(s_aurora_outer, lv_color_hex(0x00F0FF), 0);
        lv_obj_set_style_bg_opa(s_aurora_outer, LV_OPA_30, 0);
        lv_obj_set_style_radius(s_aurora_outer, 38, 0);
        lv_obj_set_style_border_width(s_aurora_outer, 0, 0);
        lv_obj_clear_flag(s_aurora_outer, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_move_background(s_aurora_outer);
    }
    if (!s_aurora_inner) {
        s_aurora_inner = lv_obj_create(parent);
        apply_no_shadow(s_aurora_inner);
        lv_obj_set_size(s_aurora_inner, 256, 52);
        lv_obj_align_to(s_aurora_inner, capsule, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(s_aurora_inner, lv_color_hex(0x00F0FF), 0);
        lv_obj_set_style_bg_opa(s_aurora_inner, LV_OPA_40, 0);
        lv_obj_set_style_radius(s_aurora_inner, 26, 0);
        lv_obj_set_style_border_width(s_aurora_inner, 0, 0);
        lv_obj_clear_flag(s_aurora_inner, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_move_background(s_aurora_inner);
    }
    /* Ensure glow is just behind capsule (above wallpaper) */
    lv_obj_move_background(s_aurora_outer);
    lv_obj_move_background(s_aurora_inner);
    lv_obj_clear_flag(s_aurora_outer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_aurora_inner, LV_OBJ_FLAG_HIDDEN);

    /* Slow dreamy hue rotation (8s) */
    lv_anim_t a_color;
    lv_anim_init(&a_color);
    lv_anim_set_var(&a_color, capsule);
    lv_anim_set_exec_cb(&a_color, aurora_color_cb);
    lv_anim_set_values(&a_color, 0, 360);
    lv_anim_set_duration(&a_color, 8000);
    lv_anim_set_repeat_count(&a_color, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a_color);

    /* Gentle breathing (4s in + 4s out) */
    lv_anim_t a_breath;
    lv_anim_init(&a_breath);
    lv_anim_set_var(&a_breath, capsule);
    lv_anim_set_exec_cb(&a_breath, aurora_breath_cb);
    lv_anim_set_values(&a_breath, 0, 100);
    lv_anim_set_duration(&a_breath, 4000);
    lv_anim_set_path_cb(&a_breath, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&a_breath, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_duration(&a_breath, 4000);
    lv_anim_start(&a_breath);
}

/* Stop aurora glow: remove animations and hide glow layers */
static void stop_aurora_animation(lv_obj_t * capsule)
{
    if (!capsule) return;
    lv_anim_delete(capsule, aurora_color_cb);
    lv_anim_delete(capsule, aurora_breath_cb);
    /* Hide glow layers (keep them allocated for next time) */
    if (s_aurora_outer) lv_obj_add_flag(s_aurora_outer, LV_OBJ_FLAG_HIDDEN);
    if (s_aurora_inner) lv_obj_add_flag(s_aurora_inner, LV_OBJ_FLAG_HIDDEN);
}

static void build_island(lv_obj_t * parent, island_t * isl)
{
    isl->capsule = lv_obj_create(parent);
    apply_no_shadow(isl->capsule);
    lv_obj_set_size(isl->capsule, 240, 36);
    lv_obj_align(isl->capsule, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(isl->capsule, lv_color_hex(0x131828), 0);
    lv_obj_set_style_bg_opa(isl->capsule, LV_OPA_60, 0);
    lv_obj_set_style_border_color(isl->capsule, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(isl->capsule, 1, 0);
    lv_obj_set_style_radius(isl->capsule, 18, 0);
    lv_obj_clear_flag(isl->capsule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(isl->capsule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(isl->capsule, [](lv_event_t * e) {
        island_t * isl = (island_t *)lv_event_get_user_data(e);
        if (isl) toggle_island(isl);
    }, LV_EVENT_CLICKED, isl);

    /* Aurara glow is started/stopped dynamically in refresh_ai_status_ui()
     * based on AI dialogue mode. Capsule starts plain. */

    isl->led_wrapper = lv_obj_create(isl->capsule);
    apply_no_shadow(isl->led_wrapper);
    lv_obj_set_size(isl->led_wrapper, 32, 32);
    lv_obj_align(isl->led_wrapper, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_opa(isl->led_wrapper, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(isl->led_wrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(isl->led_wrapper, LV_OBJ_FLAG_EVENT_BUBBLE);

    isl->led_glow = lv_obj_create(isl->led_wrapper);
    apply_no_shadow(isl->led_glow);
    lv_obj_set_size(isl->led_glow, 12, 12);
    lv_obj_center(isl->led_glow);
    lv_obj_set_style_bg_color(isl->led_glow, lv_color_hex(0x39D98A), 0);
    lv_obj_set_style_bg_opa(isl->led_glow, LV_OPA_40, 0);
    lv_obj_set_style_radius(isl->led_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(isl->led_glow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(isl->led_glow, LV_OBJ_FLAG_EVENT_BUBBLE);

    isl->led_core = lv_obj_create(isl->led_wrapper);
    apply_no_shadow(isl->led_core);
    lv_obj_set_size(isl->led_core, 8, 8);
    lv_obj_center(isl->led_core);
    lv_obj_set_style_bg_color(isl->led_core, lv_color_hex(0x39D98A), 0);
    lv_obj_set_style_bg_opa(isl->led_core, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(isl->led_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(isl->led_core, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(isl->led_core, LV_OBJ_FLAG_EVENT_BUBBLE);

    isl->status_lbl = lv_label_create(isl->capsule);
    lv_label_set_text(isl->status_lbl, "AI 待命中");
    lv_obj_set_style_text_color(isl->status_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(isl->status_lbl, &s_font_cjk_fallback, 0);
    lv_obj_align(isl->status_lbl, LV_ALIGN_LEFT_MID, 46, 0);
    lv_obj_add_flag(isl->status_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    isl->details_cnt = lv_obj_create(isl->capsule);
    apply_no_shadow(isl->details_cnt);
    lv_obj_set_size(isl->details_cnt, 320, 190);
    lv_obj_align(isl->details_cnt, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_opa(isl->details_cnt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(isl->details_cnt, 0, 0);
    lv_obj_clear_flag(isl->details_cnt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(isl->details_cnt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(isl->details_cnt, LV_OBJ_FLAG_EVENT_BUBBLE);

    isl->telemetry_title = lv_label_create(isl->details_cnt);
    lv_label_set_text(isl->telemetry_title, "SYSTEM TELEMETRY");
    lv_obj_set_style_text_color(isl->telemetry_title, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_style_text_font(isl->telemetry_title, &lv_font_montserrat_12, 0);
    lv_obj_align(isl->telemetry_title, LV_ALIGN_TOP_LEFT, 10, 0);
    lv_obj_add_flag(isl->telemetry_title, LV_OBJ_FLAG_EVENT_BUBBLE);

    const char *init_texts[] = {"Memory: -- KB free", "Storage: --", "Wi-Fi: --", "Sensor: --", "BLE: --"};
    for (int i = 0; i < 5; i++) {
        isl->detail_lbls[i] = lv_label_create(isl->details_cnt);
        lv_obj_set_style_text_color(isl->detail_lbls[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(isl->detail_lbls[i], &s_font_cjk_fallback, 0);
        lv_label_set_recolor(isl->detail_lbls[i], true);
        lv_label_set_text(isl->detail_lbls[i], init_texts[i]);
        lv_obj_align(isl->detail_lbls[i], LV_ALIGN_TOP_LEFT, 10, 26 + i * 30);
        lv_obj_add_flag(isl->detail_lbls[i], LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    /* AI Title */
    isl->ai_title = lv_label_create(isl->details_cnt);
    lv_label_set_text(isl->ai_title, "小果 AI 助手");
    lv_obj_set_style_text_font(isl->ai_title, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(isl->ai_title, lv_color_hex(0x03DAC6), 0);
    lv_obj_align(isl->ai_title, LV_ALIGN_TOP_LEFT, 10, 0);
    lv_obj_add_flag(isl->ai_title, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(isl->ai_title, LV_OBJ_FLAG_HIDDEN);

    /* AI Chat Subtitle Label */
    isl->ai_chat_lbl = lv_label_create(isl->details_cnt);
    lv_label_set_text(isl->ai_chat_lbl, "开启对话以开始...");
    lv_obj_set_style_text_font(isl->ai_chat_lbl, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(isl->ai_chat_lbl, lv_color_hex(0xE6ECF5), 0);
    lv_label_set_long_mode(isl->ai_chat_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(isl->ai_chat_lbl, 300);
    lv_obj_align(isl->ai_chat_lbl, LV_ALIGN_TOP_LEFT, 10, 26);
    lv_obj_add_flag(isl->ai_chat_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(isl->ai_chat_lbl, LV_OBJ_FLAG_HIDDEN);

    /* AI Progress Bar */
    isl->ai_progress_bar = lv_bar_create(isl->details_cnt);
    lv_obj_set_size(isl->ai_progress_bar, 300, 16);
    lv_obj_align(isl->ai_progress_bar, LV_ALIGN_TOP_LEFT, 10, 110);
    lv_obj_set_style_bg_color(isl->ai_progress_bar, lv_color_hex(0x192543), LV_PART_MAIN);
    lv_obj_set_style_bg_color(isl->ai_progress_bar, lv_color_hex(0x63B3ED), LV_PART_INDICATOR);
    lv_obj_set_style_radius(isl->ai_progress_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(isl->ai_progress_bar, 8, LV_PART_INDICATOR);
    lv_obj_add_flag(isl->ai_progress_bar, LV_OBJ_FLAG_HIDDEN);

    /* AI Progress Label */
    isl->ai_progress_lbl = lv_label_create(isl->details_cnt);
    lv_label_set_text(isl->ai_progress_lbl, "训练进度: 0%");
    lv_obj_set_style_text_font(isl->ai_progress_lbl, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(isl->ai_progress_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(isl->ai_progress_lbl, LV_ALIGN_TOP_LEFT, 10, 80);
    lv_obj_add_flag(isl->ai_progress_lbl, LV_OBJ_FLAG_HIDDEN);

    /* AI Status Label below the dialogue */
    isl->ai_status_lbl = lv_label_create(isl->details_cnt);
    lv_label_set_long_mode(isl->ai_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(isl->ai_status_lbl, 300);
    lv_label_set_text(isl->ai_status_lbl, "后台 AI 自动监控就绪");
    lv_obj_set_style_text_font(isl->ai_status_lbl, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(isl->ai_status_lbl, lv_color_hex(0x91A0B8), 0); // Clean gray-blue status text
    lv_obj_align(isl->ai_status_lbl, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_add_flag(isl->ai_status_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(isl->ai_status_lbl, LV_OBJ_FLAG_HIDDEN);

    if (isl == &m_island) {
        xiaozhi_chat_label = isl->ai_chat_lbl;
        siri_state_label = isl->status_lbl;
        ai_training_progress_bar = isl->ai_progress_bar;
        ai_training_progress_label = isl->ai_progress_lbl;
    }

    isl->expanded = false;
}

static island_t * active_island(void)
{
    lv_obj_t * act = lv_scr_act();
    if (act == scr_main) return &m_island;
    return &s_island;
}

static void toggle_island(island_t * isl)
{
    if (!isl || !isl->capsule) return;

    isl->expanded = !isl->expanded;

    if (isl->status_lbl) lv_obj_add_flag(isl->status_lbl, LV_OBJ_FLAG_HIDDEN);
    if (isl->led_wrapper) lv_obj_add_flag(isl->led_wrapper, LV_OBJ_FLAG_HIDDEN);
    if (isl->details_cnt) lv_obj_add_flag(isl->details_cnt, LV_OBJ_FLAG_HIDDEN);

    lv_anim_delete(isl->capsule, island_size_anim_cb);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, isl->capsule);
    lv_anim_set_exec_cb(&a, island_size_anim_cb);
    lv_anim_set_ready_cb(&a, [](lv_anim_t * a) {
        island_t * isl = (island_t *)lv_anim_get_user_data(a);
        if (!isl) return;
        if (isl->expanded) {
            if (isl->details_cnt) {
                lv_obj_remove_flag(isl->details_cnt, LV_OBJ_FLAG_HIDDEN);
                update_island_details(isl);
            }
        } else {
            if (isl->status_lbl) lv_obj_remove_flag(isl->status_lbl, LV_OBJ_FLAG_HIDDEN);
            if (isl->led_wrapper) lv_obj_remove_flag(isl->led_wrapper, LV_OBJ_FLAG_HIDDEN);
        }
    });
    lv_anim_set_user_data(&a, isl);

    if (isl->expanded) {
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_duration(&a, 350);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    } else {
        lv_anim_set_values(&a, 100, 0);
        lv_anim_set_duration(&a, 300);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    }

    lv_anim_start(&a);
}

static void update_island_view_mode(island_t * isl) {
    if (!isl || !isl->details_cnt) return;

    bool is_ai_mode = (ai_service_running || g_training_progress >= 0 || ptt_pressed);
    
    // 1. Show/Hide Telemetry UI
    if (is_ai_mode) {
        if (isl->telemetry_title) lv_obj_add_flag(isl->telemetry_title, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 5; i++) {
            if (isl->detail_lbls[i]) lv_obj_add_flag(isl->detail_lbls[i], LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (isl->telemetry_title) lv_obj_remove_flag(isl->telemetry_title, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 5; i++) {
            if (isl->detail_lbls[i]) lv_obj_remove_flag(isl->detail_lbls[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 2. Show/Hide AI UI
    if (is_ai_mode) {
        if (isl->ai_title) lv_obj_remove_flag(isl->ai_title, LV_OBJ_FLAG_HIDDEN);
        if (isl->ai_status_lbl) lv_obj_remove_flag(isl->ai_status_lbl, LV_OBJ_FLAG_HIDDEN);
        if (g_training_progress >= 0) {
            // Training state
            if (isl->ai_chat_lbl) lv_obj_add_flag(isl->ai_chat_lbl, LV_OBJ_FLAG_HIDDEN);
            if (isl->ai_progress_bar) {
                lv_obj_remove_flag(isl->ai_progress_bar, LV_OBJ_FLAG_HIDDEN);
                lv_bar_set_value(isl->ai_progress_bar, g_training_progress, LV_ANIM_OFF);
            }
            if (isl->ai_progress_lbl) {
                lv_obj_remove_flag(isl->ai_progress_lbl, LV_OBJ_FLAG_HIDDEN);
                if (g_training_tot_epoch > 0) {
                    lv_label_set_text_fmt(isl->ai_progress_lbl, "训练中: %d/%d Epochs (%d%%)", g_training_cur_epoch, g_training_tot_epoch, g_training_progress);
                } else {
                    lv_label_set_text_fmt(isl->ai_progress_lbl, "进度: %d%%", g_training_progress);
                }
            }
        } else {
            // Chat state
            if (isl->ai_progress_bar) lv_obj_add_flag(isl->ai_progress_bar, LV_OBJ_FLAG_HIDDEN);
            if (isl->ai_progress_lbl) lv_obj_add_flag(isl->ai_progress_lbl, LV_OBJ_FLAG_HIDDEN);
            if (isl->ai_chat_lbl) lv_obj_remove_flag(isl->ai_chat_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (isl->ai_title) lv_obj_add_flag(isl->ai_title, LV_OBJ_FLAG_HIDDEN);
        if (isl->ai_status_lbl) lv_obj_add_flag(isl->ai_status_lbl, LV_OBJ_FLAG_HIDDEN);
        if (isl->ai_chat_lbl) lv_obj_add_flag(isl->ai_chat_lbl, LV_OBJ_FLAG_HIDDEN);
        if (isl->ai_progress_bar) lv_obj_add_flag(isl->ai_progress_bar, LV_OBJ_FLAG_HIDDEN);
        if (isl->ai_progress_lbl) lv_obj_add_flag(isl->ai_progress_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_island_details(island_t * isl)
{
    if (!isl || !isl->details_cnt) return;

    update_island_view_mode(isl);

    bool is_ai_mode = (ai_service_running || g_training_progress >= 0 || ptt_pressed);
    if (!is_ai_mode) {
        uint32_t free_heap = esp_get_free_heap_size() / 1024;
        char buf[64];
        snprintf(buf, sizeof(buf), "#39D98A ●# Memory: #FFFFFF %u KB free#", (unsigned int)free_heap);
        if (isl->detail_lbls[0]) lv_label_set_text(isl->detail_lbls[0], buf);

        bool sd_ready = sd_card_bg_is_ready();
        if (isl->detail_lbls[1]) {
            lv_label_set_text(isl->detail_lbls[1], sd_ready ? "#39D98A ●# Storage: #FFFFFF SD Mounted#" : "#FFB84D ●# Storage: #FFFFFF SD Disconnected#");
        }

        bool wifi_connected = wifi_is_connected();
        if (isl->detail_lbls[2]) {
            if (wifi_connected) {
                lv_label_set_text(isl->detail_lbls[2], "#39D98A ●# Wi-Fi: #FFFFFF Connected#");
            } else if (wifi_conn_status == 1) {
                lv_label_set_text(isl->detail_lbls[2], "#4AA3FF ●# Wi-Fi: #FFFFFF Connecting...#");
            } else {
                lv_label_set_text(isl->detail_lbls[2], "#FFB84D ●# Wi-Fi: #FFFFFF Disconnected#");
            }
        }

        if (isl->detail_lbls[3]) {
            lv_label_set_text(isl->detail_lbls[3], "#39D98A ●# Sensor: #FFFFFF Online#");
        }

        if (isl->detail_lbls[4]) {
            lv_label_set_text(isl->detail_lbls[4], "#39D98A ●# BLE Mesh: #FFFFFF Controller Active#");
        }
    }
}

/* ===== Smart Island (灵动岛) Animation Callbacks ===== */

static void island_size_anim_cb(void * var, int32_t v) {
    lv_obj_t * obj = (lv_obj_t *)var;
    if (!obj) return;

    int32_t w = 240 + ((360 - 240) * v / 100);
    int32_t h = 36 + ((240 - 36) * v / 100);
    int32_t r = 18 + ((24 - 18) * v / 100);

    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, r, 0);
}

static void island_flow_timer_cb(lv_timer_t * timer) {
    LV_UNUSED(timer);
    lv_obj_t * act = lv_scr_act();
    // Only animate if the active screen is scr_main or scr_startup
    if (act != scr_main && act != scr_startup) return;

    island_t * isl = (act == scr_main) ? &m_island : &s_island;
    if (!isl || !isl->capsule) return;

    // Save CPU: only flow border color when the island is expanded or AI mode is active.
    // Otherwise, set a static premium border color and exit early.
    bool is_ai_active = (ai_service_running || g_training_progress >= 0 || ptt_pressed);
    if (!isl->expanded && !is_ai_active) {
        lv_obj_set_style_border_color(isl->capsule, lv_color_hex(0x2A3A5A), 0);
        return;
    }

    lv_obj_t * obj = isl->capsule;

    static int s_border_flow_hue = 0;
    s_border_flow_hue = (s_border_flow_hue + 24) % 360;
    int v = s_border_flow_hue;

    lv_color_t color;
    if (v < 120) {
        int r = 0x39 + ((0x00 - 0x39) * v / 120);
        int g = 0xD9 + ((0xF0 - 0xD9) * v / 120);
        int b = 0x8A + ((0xFF - 0x8A) * v / 120);
        color = lv_color_make(r, g, b);
    } else if (v < 240) {
        int val = v - 120;
        int r = 0x00 + ((0xFF - 0x00) * val / 120);
        int g = 0xF0 + ((0x00 - 0xF0) * val / 120);
        int b = 0xFF + ((0x7F - 0xFF) * val / 120);
        color = lv_color_make(r, g, b);
    } else {
        int val = v - 240;
        int r = 0xFF + ((0x39 - 0xFF) * val / 120);
        int g = 0x00 + ((0xD9 - 0x00) * val / 120);
        int b = 0x7F + ((0x8A - 0x7F) * val / 120);
        color = lv_color_make(r, g, b);
    }

    lv_obj_set_style_border_color(obj, color, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_90, 0);
}

/* ===== Startup Neon Text Animations ===== */

static void neon_anim_glow_cb(void * var, int32_t v) {
    lv_obj_t * obj = (lv_obj_t *)var;
    if (obj) {
        lv_obj_set_style_text_opa(obj, v, 0);
    }
}

static void neon_anim_aura_cb(void * var, int32_t v) {
    lv_obj_t * obj = (lv_obj_t *)var;
    if (obj) {
        lv_obj_set_style_text_opa(obj, v, 0);
    }
}

static void neon_anim_glow_x_cb(void * var, int32_t v) {
    lv_obj_t * obj = (lv_obj_t *)var;
    if (obj) {
        lv_obj_set_style_translate_x(obj, v, 0);
    }
}

static void neon_anim_aura_y_cb(void * var, int32_t v) {
    lv_obj_t * obj = (lv_obj_t *)var;
    if (obj) {
        lv_obj_set_style_translate_y(obj, v, 0);
    }
}

static void ensure_wifi_ready(void) {
    if(!wifi_initialized) {
        esp_netif_t *netif = esp_netif_create_default_wifi_sta();

        // 使用 DHCP 自动获取 IP (兼容不同网段的 WiFi)

        // 设置 DNS
        esp_netif_dns_info_t dns_info;
        inet_pton(AF_INET, P4_DNS, &dns_info.ip);
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
        esp_netif_dns_info_t dns_backup;
        inet_pton(AF_INET, P4_DNS_BACKUP, &dns_backup.ip);
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_backup);
        ESP_LOGI("WIFI", "DNS main=%s backup=%s", P4_DNS, P4_DNS_BACKUP);

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();

        // Note: Removed early Wi-Fi power save and TX power configuration APIs
        // to prevent block lock contention / spinlock panic on ESP32-P4 with ESP-Hosted.
        wifi_initialized = true;

        // 移除了此处的立即自动重连。重连工作改由扫描完成后进行匹配连接，从而实现“有信号才重连”的逻辑。
    }
}

static void render_wifi_scan_results(void) {
    uint16_t number;
    bool scan_ok;

    if (wifi_scan_mutex == NULL || xSemaphoreTake(wifi_scan_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW("WIFI", "render_wifi: mutex take failed, showing Scan Busy");
        lv_obj_clean(wifi_list);
        lv_obj_t * lbl = lv_label_create(wifi_list);
        lv_label_set_text(lbl, "Scan Busy");
        return;
    }

    number = wifi_scan_result_count;
    scan_ok = wifi_scan_last_ok;

    if (number > 10) {
        number = 10;
    }

    ESP_LOGI("WIFI", "render_wifi: number=%d scan_ok=%d", number, scan_ok);

    lv_obj_clean(wifi_list);

    if (!scan_ok) {
        xSemaphoreGive(wifi_scan_mutex);
        lv_obj_t * lbl = lv_label_create(wifi_list);
        lv_label_set_text(lbl, "Scan Failed");
        return;
    }

    if (number == 0) {
        xSemaphoreGive(wifi_scan_mutex);
        lv_obj_t * lbl = lv_label_create(wifi_list);
        lv_label_set_text(lbl, "No Networks Found");
        return;
    }

    int btn_index = 0;
    for (uint16_t i = 0; i < number; i++) {
        if (wifi_scan_ssids[i][0] == '\0') {
            continue;
        }
        lv_obj_t * btn = lv_btn_create(wifi_list);
        lv_obj_set_size(btn, 320, 40);
        lv_obj_set_pos(btn, 10, btn_index * 45);
        
        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text(lbl, wifi_scan_ssids[i]);
        lv_obj_set_style_text_font(lbl, &s_font_cjk_fallback, 0); // Allow CJK SSID names
        lv_obj_center(lbl);
        
        lv_obj_add_event_cb(btn, wifi_list_btn_cb, LV_EVENT_CLICKED, NULL);
        btn_index++;
    }

    xSemaphoreGive(wifi_scan_mutex);
}

static void render_wifi_scan_results_async(void * user_data) {
    LV_UNUSED(user_data);
    render_wifi_scan_results();
}

static void wifi_scan_task(void * arg) {
    LV_UNUSED(arg);

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

#ifdef CONFIG_ELEC_NOSE_GATEWAY_ENABLE
        // 电子鼻网关 AI 联网防御：若正在进行关键联网分析，暂缓并挂起扫描，保障数据通道优先
        if (electronic_nose_gateway_get_state() == GATEWAY_STATE_PROCESSING) {
            ESP_LOGW("WIFI", "Gateway is busy with AI analysis. Postponing WiFi scan for 1.5s...");
            vTaskDelay(pdMS_TO_TICKS(1500));
            if (wifi_scan_task_handle) {
                xTaskNotifyGive(wifi_scan_task_handle);
            }
            continue;
        }
#endif

        ensure_wifi_ready();
        wifi_scan_in_progress = true;
        wifi_scan_last_ok = false;

        // 不再在已连接 (wifi_conn_status == 2) 状态下强制断开 WiFi 链接
        // 从而彻底防止 AI 网关网络被打断，且保留平滑静默扫描功能
        wifi_disconnected_for_scan = false;
        
        if (wifi_conn_status == 1) {
            ESP_LOGW("WIFI", "WiFi is connecting... Waiting 1s before scanning to avoid state clash");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        ESP_LOGI("WIFI", "WiFi Scan triggered (status=%d), scanning...", (int)wifi_conn_status);

        wifi_scan_done = false;
        ESP_LOGI("WIFI", "Starting WiFi scan");
        esp_err_t err = esp_wifi_scan_start(NULL, false);
        if (err == ESP_OK) {
            int wait_count = 0;
            while (!wifi_scan_done && wait_count < 50) {
                vTaskDelay(pdMS_TO_TICKS(100));
                wait_count++;
            }
            if (!wifi_scan_done) {
                ESP_LOGW("WIFI", "Scan wait timeout (%d ms)", wait_count * 100);
                err = ESP_ERR_TIMEOUT;
            } else {
                ESP_LOGI("WIFI", "Scan completed in %d ms", wait_count * 100);
            }
        } else {
            ESP_LOGE("WIFI", "Scan start failed: %s", esp_err_to_name(err));
        }

        if (wifi_scan_mutex && xSemaphoreTake(wifi_scan_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            memset(wifi_scan_ssids, 0, sizeof(wifi_scan_ssids));
            wifi_scan_result_count = 0;

            if (err == ESP_OK) {
                uint16_t number = 10;
                wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * number);
                if (ap_records != NULL) {
                    memset(ap_records, 0, sizeof(wifi_ap_record_t) * number);
                    if (esp_wifi_scan_get_ap_records(&number, ap_records) == ESP_OK) {
                        ESP_LOGI("WIFI", "Scan found %u APs", number);
                        wifi_scan_last_ok = true;
                        for (uint16_t i = 0; i < number && wifi_scan_result_count < 60; i++) {
                            if (ap_records[i].ssid[0] == '\0') {
                                continue;
                            }
                            strncpy(wifi_scan_ssids[wifi_scan_result_count], (const char *)ap_records[i].ssid, sizeof(wifi_scan_ssids[0]) - 1);
                            wifi_scan_ssids[wifi_scan_result_count][sizeof(wifi_scan_ssids[0]) - 1] = '\0';
                            wifi_scan_result_count++;
                        }
                    }
                    free(ap_records);
                }
            }

            xSemaphoreGive(wifi_scan_mutex);
        }

        wifi_scan_in_progress = false;
        ESP_LOGI("WIFI", "Scan complete, count=%d, ok=%d", wifi_scan_result_count, wifi_scan_last_ok);

        // “扫描匹配式自动重连”：若当前处于未联网状态，且扫描成功，则检查是否有保存的 SSID 存在于扫描结果中
        if ((wifi_conn_status == 0 || wifi_conn_status == 3) && wifi_scan_last_ok) {
            char saved_ssid[33] = {}, saved_pass[65] = {};
            if (load_wifi_creds(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass))) {
                bool found = false;
                if (wifi_scan_mutex && xSemaphoreTake(wifi_scan_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    for (int i = 0; i < wifi_scan_result_count; i++) {
                        if (strcmp(wifi_scan_ssids[i], saved_ssid) == 0) {
                            found = true;
                            break;
                        }
                    }
                    xSemaphoreGive(wifi_scan_mutex);
                }
                if (found) {
                    ESP_LOGI("WIFI", "Scan auto-connect: Saved AP '%s' detected. Connecting...", saved_ssid);
                    wifi_config_t wifi_config = {};
                    strncpy((char *)wifi_config.sta.ssid, saved_ssid, sizeof(wifi_config.sta.ssid) - 1);
                    strncpy((char *)wifi_config.sta.password, saved_pass, sizeof(wifi_config.sta.password) - 1);
                    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
                    wifi_conn_status = 1;
                    wifi_status_timer_ticks = 0;
                    esp_wifi_connect();
                } else {
                    ESP_LOGI("WIFI", "Scan auto-connect: Saved AP '%s' NOT detected in scan. Waiting for manual connection.", saved_ssid);
                    wifi_conn_status = 3;
                }
            }
        }

        // 不在扫描结束后自动重连(避免与用户手动选新SSID冲突),
        // 改为由 wifi_list_close_btn_cb 处理重连
        if (wifi_disconnected_for_scan) {
            wifi_disconnected_for_scan = false;
        }

        lv_async_call(render_wifi_scan_results_async, NULL);
    }
}

static void request_wifi_scan(void) {
    ESP_LOGI("WIFI", "request_wifi_scan: start");

    if (wifi_scan_mutex == NULL) {
        wifi_scan_mutex = xSemaphoreCreateMutex();
    }
    if (wifi_scan_task_handle == NULL) {
        xTaskCreatePinnedToCore(wifi_scan_task, "wifi_scan", 12288, NULL, 2, &wifi_scan_task_handle, 0);
        if (wifi_scan_task_handle == NULL) {
            ESP_LOGE("WIFI", "Failed to create scan task");
            wifi_scan_in_progress = false;
            return;
        }
    }

    wifi_scan_in_progress = true;
    xTaskNotifyGive(wifi_scan_task_handle);
    ESP_LOGI("WIFI", "request_wifi_scan: scan task notified");
}

static void btn_open_wifi_cb(lv_event_t * e) {
    LV_UNUSED(e);
    lv_obj_move_foreground(wifi_list_modal);
    lv_obj_remove_flag(wifi_list_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(wifi_list);
    lv_obj_t * lbl = lv_label_create(wifi_list);
    lv_label_set_text(lbl, "Scanning...");
    lv_obj_center(lbl);
    
    // Directly call request_wifi_scan instead of using a one-shot timer
    request_wifi_scan();
}

static void create_wifi_modals(void) {
    lv_obj_t * overlay_parent = lv_layer_top();

    wifi_list_modal = lv_obj_create(overlay_parent);
    apply_no_shadow(wifi_list_modal);
    lv_obj_set_size(wifi_list_modal, 400, 300);
    lv_obj_align(wifi_list_modal, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(wifi_list_modal, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(wifi_list_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(wifi_list_modal, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(wifi_list_modal, 1, 0);
    lv_obj_set_style_radius(wifi_list_modal, 12, 0);
    lv_obj_add_flag(wifi_list_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * list_title = lv_label_create(wifi_list_modal);
    lv_label_set_text(list_title, "可用 Wi-Fi 网络");
    lv_obj_set_style_text_font(list_title, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(list_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(list_title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t * close_btn = lv_btn_create(wifi_list_modal);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "X");
    lv_obj_add_event_cb(close_btn, wifi_list_close_btn_cb, LV_EVENT_CLICKED, NULL);

    wifi_list = lv_obj_create(wifi_list_modal);
    lv_obj_set_size(wifi_list, 360, 220);
    lv_obj_align(wifi_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_scroll_dir(wifi_list, LV_DIR_VER);

    wifi_pass_modal = lv_obj_create(overlay_parent);
    apply_no_shadow(wifi_pass_modal);
    lv_obj_set_size(wifi_pass_modal, 400, 300);
    lv_obj_align(wifi_pass_modal, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(wifi_pass_modal, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(wifi_pass_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(wifi_pass_modal, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(wifi_pass_modal, 1, 0);
    lv_obj_set_style_radius(wifi_pass_modal, 12, 0);
    lv_obj_add_flag(wifi_pass_modal, LV_OBJ_FLAG_HIDDEN);

    wifi_pass_title = lv_label_create(wifi_pass_modal);
    lv_label_set_text(wifi_pass_title, "连接 Wi-Fi");
    lv_obj_set_style_text_font(wifi_pass_title, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(wifi_pass_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(wifi_pass_title, LV_ALIGN_TOP_MID, 0, 0);

    ta_pass = lv_textarea_create(wifi_pass_modal);
    lv_textarea_set_placeholder_text(ta_pass, "Password...");
    lv_textarea_set_password_mode(ta_pass, true);
    lv_obj_set_width(ta_pass, 300);
    lv_obj_align(ta_pass, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_add_event_cb(ta_pass, ta_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t * btn_conn = lv_btn_create(wifi_pass_modal);
    lv_obj_align(btn_conn, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
    lv_obj_add_event_cb(btn_conn, wifi_connect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_conn = lv_label_create(btn_conn);
    lv_label_set_text(lbl_conn, "Connect");

    lv_obj_t * btn_back = lv_btn_create(wifi_pass_modal);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 20, -10);
    lv_obj_add_event_cb(btn_back, wifi_pass_back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");

    kb = lv_keyboard_create(overlay_parent);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

/* ===== Wallpaper Functions ===== */

static void wallpaper_select_cb(lv_event_t * e) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    const char * path = (const char *)lv_obj_get_user_data(btn);
    if (path) {
        set_wallpaper(path);
    }
    lv_obj_add_flag(wallpaper_modal, LV_OBJ_FLAG_HIDDEN);
}

static void wallpaper_list_close_btn_cb(lv_event_t * e) {
    lv_obj_add_flag(wallpaper_modal, LV_OBJ_FLAG_HIDDEN);
}

static void open_wallpaper_list(void) {
    lv_obj_clean(wallpaper_list);

    if (!sd_card_bg_is_ready()) {
        lv_obj_t * lbl = lv_label_create(wallpaper_list);
        lv_label_set_text(lbl, "SD Card Not Ready");
        lv_obj_center(lbl);
        lv_obj_move_foreground(wallpaper_modal);
        lv_obj_remove_flag(wallpaper_modal, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    DIR *dir = opendir("/sdcard");
    if (!dir) {
        lv_obj_t * lbl = lv_label_create(wallpaper_list);
        lv_label_set_text(lbl, "Cannot Open /sdcard");
        lv_obj_center(lbl);
        lv_obj_move_foreground(wallpaper_modal);
        lv_obj_remove_flag(wallpaper_modal, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
            if (wallpaper_ends_with(entry->d_name, ".jpg") || wallpaper_ends_with(entry->d_name, ".jpeg")) {
                char full_path[256];
                snprintf(full_path, sizeof(full_path), "/sdcard/%s", entry->d_name);

                char *path_copy = (char *)malloc(strlen(full_path) + 1);
                if (path_copy) {
                    strcpy(path_copy, full_path);

                    lv_obj_t * item_btn = lv_btn_create(wallpaper_list);
                    lv_obj_set_size(item_btn, 320, 40);
                    lv_obj_set_pos(item_btn, 10, count * 45);
                    
                    lv_obj_t * lbl = lv_label_create(item_btn);
                    lv_label_set_text(lbl, entry->d_name);
                    lv_obj_center(lbl);
                    
                    lv_obj_set_user_data(item_btn, path_copy);
                    lv_obj_add_event_cb(item_btn, wallpaper_select_cb, LV_EVENT_CLICKED, NULL);
                    lv_obj_add_event_cb(item_btn, [](lv_event_t * e) {
                        void *data = lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
                        if (data) free(data);
                    }, LV_EVENT_DELETE, NULL);
                    count++;
                }
            }
        }
    }
    closedir(dir);

    if (count == 0) {
        lv_obj_t * lbl = lv_label_create(wallpaper_list);
        lv_label_set_text(lbl, "No JPEG files found");
        lv_obj_center(lbl);
    }

    lv_obj_move_foreground(wallpaper_modal);
    lv_obj_remove_flag(wallpaper_modal, LV_OBJ_FLAG_HIDDEN);
}

static void create_wallpaper_modal(void) {
    lv_obj_t * overlay_parent = lv_layer_top();

    wallpaper_modal = lv_obj_create(overlay_parent);
    apply_no_shadow(wallpaper_modal);
    lv_obj_set_size(wallpaper_modal, 400, 400);
    lv_obj_align(wallpaper_modal, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(wallpaper_modal, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(wallpaper_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(wallpaper_modal, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(wallpaper_modal, 1, 0);
    lv_obj_set_style_radius(wallpaper_modal, 12, 0);
    lv_obj_add_flag(wallpaper_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(wallpaper_modal);
    lv_label_set_text(title, "Select Wallpaper");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t * close_btn = lv_btn_create(wallpaper_modal);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_set_style_radius(close_btn, 18, 0);
    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "X");
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, wallpaper_list_close_btn_cb, LV_EVENT_CLICKED, NULL);

    wallpaper_list = lv_obj_create(wallpaper_modal);
    lv_obj_set_size(wallpaper_list, 360, 320);
    lv_obj_align(wallpaper_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_scroll_dir(wallpaper_list, LV_DIR_VER);
}

#if 0 /* CONFIG_ELEC_NOSE_GATEWAY_ENABLE - Removed permanently per user request */
static void ai_analyze_btn_cb(lv_event_t * e) {
    char sensor_json[512];
    bool has_data = false;

    extern float g_temperature;
    extern float g_humidity;

    if (s_udp_data_mutex != NULL && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_udp_node1.valid) {
            int odor_int = (int)(s_udp_node1.odor * 100);
            int hcho_int = (int)(s_udp_node1.hcho * 100);
            int co_int = (int)(s_udp_node1.co * 100);
            int voc_int = (int)(s_udp_node1.voc * 10);
            int conf_int = (int)(s_udp_node1.conf * 100);
            int temp_int = (int)(g_temperature * 10);
            int humi_int = (int)(g_humidity * 10);
            snprintf(sensor_json, sizeof(sensor_json),
                "{\"node\":\"S3_Receiver\",\"odor\":%d.%02d,\"hcho\":%d.%02d,\"co\":%d.%02d,"
                "\"voc\":%d.%d,\"co2\":%d,\"pred\":%d,\"class\":\"%s\",\"conf\":%d.%02d,"
                "\"fresh\":%d,\"temp\":%d.%d,\"humi\":%d.%d}",
                odor_int / 100, odor_int % 100,
                hcho_int / 100, hcho_int % 100,
                co_int / 100, co_int % 100,
                voc_int / 10, voc_int % 10,
                s_udp_node1.co2, s_udp_node1.pred,
                s_udp_node1.sensor_class,
                conf_int / 100, conf_int % 100,
                s_udp_node1.fresh,
                temp_int / 10, temp_int % 10,
                humi_int / 10, humi_int % 10);
            has_data = true;
        }
        xSemaphoreGive(s_udp_data_mutex);
    }

    if (!has_data) {
        int temp_int = (int)(g_temperature * 10);
        int humi_int = (int)(g_humidity * 10);
        snprintf(sensor_json, sizeof(sensor_json),
            "{\"node\":\"S3_Receiver\",\"odor\":0,\"hcho\":0,\"co\":0,"
            "\"voc\":8.5,\"co2\":600,\"pred\":0,\"class\":\"unknown\",\"conf\":0,"
            "\"fresh\":0,\"temp\":%d.%d,\"humi\":%d.%d}",
            temp_int / 10, temp_int % 10,
            humi_int / 10, humi_int % 10);
    }

    esp_err_t err = electronic_nose_gateway_trigger_analysis(sensor_json, strlen(sensor_json));
    if (err == ESP_OK) {
        lv_label_set_text(ai_analyze_btn_label, "Analyzing...");
        lv_obj_set_style_bg_color(ai_analyze_btn, lv_color_hex(0x7A5C00), 0);
        lv_obj_add_state(ai_analyze_btn, LV_STATE_DISABLED);
    }
}

static void ai_dialog_close_cb(lv_event_t * e) {
    if (ai_result_dialog) {
        lv_obj_del(ai_result_dialog);
        ai_result_dialog = NULL;
        s_ai_dialog_open = false;
    }
    lv_obj_clear_state(ai_analyze_btn, LV_STATE_DISABLED);
    lv_label_set_text(ai_analyze_btn_label, "AI Analysis");
    lv_obj_set_style_bg_color(ai_analyze_btn, lv_color_hex(0x4AA3FF), 0);
}

static void create_ai_result_dialog(const char * result) {
    if (s_ai_dialog_open) return;
    s_ai_dialog_open = true;

    ai_result_dialog = lv_obj_create(lv_layer_top());
    apply_no_shadow(ai_result_dialog);
    lv_obj_set_size(ai_result_dialog, 940, 540);
    lv_obj_center(ai_result_dialog);
    lv_obj_set_style_bg_color(ai_result_dialog, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(ai_result_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ai_result_dialog, lv_color_hex(0x4AA3FF), 0);
    lv_obj_set_style_border_width(ai_result_dialog, 2, 0);
    lv_obj_set_style_radius(ai_result_dialog, 18, 0);

    lv_obj_t * title = lv_label_create(ai_result_dialog);
    lv_label_set_text(title, "AI Analysis Result");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x4AA3FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 14);

    lv_obj_t * close_btn = lv_btn_create(ai_result_dialog);
    lv_obj_set_size(close_btn, 80, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -14, 12);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_center(close_lbl);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(close_btn, ai_dialog_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * text_area = lv_obj_create(ai_result_dialog);
    apply_no_shadow(text_area);
    lv_obj_set_size(text_area, 890, 440);
    lv_obj_align(text_area, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(text_area, lv_color_hex(0x151C30), 0);
    lv_obj_set_style_border_color(text_area, lv_color_hex(0x26324A), 0);
    lv_obj_set_style_border_width(text_area, 1, 0);
    lv_obj_set_style_radius(text_area, 12, 0);
    lv_obj_set_flex_flow(text_area, LV_FLEX_FLOW_COLUMN);

    ai_result_dialog_text = lv_label_create(text_area);
    lv_label_set_text(ai_result_dialog_text, result ? result : "(empty)");
    lv_label_set_long_mode(ai_result_dialog_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ai_result_dialog_text, 850);
    lv_obj_set_style_text_font(ai_result_dialog_text, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(ai_result_dialog_text, lv_color_hex(0xE6ECF5), 0);
    lv_obj_set_style_text_line_space(ai_result_dialog_text, 4, 0);
}

void ui_show_ai_analysis(const char* result) {
    if (!result) return;
    strlcpy(s_ai_result_buffer, result, sizeof(s_ai_result_buffer));

    esp_err_t lock_ret = esp_lv_adapter_lock(pdMS_TO_TICKS(1000));
    if (lock_ret == ESP_OK) {
        create_ai_result_dialog(s_ai_result_buffer);
        esp_lv_adapter_unlock();
    }
}
#endif // CONFIG_ELEC_NOSE_GATEWAY_ENABLE

/* ── Switch the shared detail screen to show a given device ── */
static void switch_to_device(int idx) {
    if (idx < 0 || idx >= MAX_DEVICE_APPS || !s_device_apps[idx].active) return;
    s_current_device_idx = idx;
    if (device_detail_title) {
        lv_label_set_text_fmt(device_detail_title, "%s Sensor Data", s_device_apps[idx].name);
    }
    if (s_detail_chart && s_chart_ser_voc) {
        lv_chart_set_all_value(s_detail_chart, s_chart_ser_voc, 0);
    }
    if (scr_device_detail) {
        lv_screen_load(scr_device_detail);
    }
}

/* ── Find existing or create a new sidebar button for a device name ── */
static device_app_t* device_app_ensure(const char *name) {
    if (!name || !name[0] || !s_device_app_mutex) return NULL;
    if (xSemaphoreTake(s_device_app_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return NULL;

    /* Find existing slot */
    device_app_t *app = NULL;
    int empty_slot = -1;
    for (int i = 0; i < MAX_DEVICE_APPS; i++) {
        if (!app && !s_device_apps[i].active && empty_slot < 0) empty_slot = i;
        if (s_device_apps[i].active && strcmp(s_device_apps[i].name, name) == 0) {
            app = &s_device_apps[i];
            break;
        }
    }
    if (!app) {
        if (empty_slot < 0) { xSemaphoreGive(s_device_app_mutex); return NULL; }
        app = &s_device_apps[empty_slot];
    }

    /* Already has a sidebar button */
    if (app->active && app->app_btn) { xSemaphoreGive(s_device_app_mutex); return app; }

    strncpy(app->name, name, sizeof(app->name) - 1);
    app->name[sizeof(app->name) - 1] = '\0';
    memset(&app->data, 0, sizeof(app->data));

    /* Create sidebar button */
    if (apps_panel) {
        int row = s_device_app_count / 2;
        int col = s_device_app_count % 2;
        int bx = 16 + col * 88;
        int by = 46 + row * 96;

        app->app_btn = lv_btn_create(apps_panel);
        lv_obj_set_size(app->app_btn, 80, 80);
        lv_obj_set_pos(app->app_btn, bx, by);
        lv_obj_set_style_bg_color(app->app_btn, lv_color_hex(0x1E2942), 0);
        lv_obj_set_style_bg_opa(app->app_btn, LV_OPA_60, 0);
        lv_obj_set_style_border_color(app->app_btn, lv_color_hex(0x39D98A), 0);
        lv_obj_set_style_border_width(app->app_btn, 1, 0);
        lv_obj_set_style_radius(app->app_btn, 20, 0);
        lv_obj_t *sym = lv_label_create(app->app_btn);
        lv_label_set_text(sym, LV_SYMBOL_REFRESH);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(sym, lv_color_hex(0x4AA3FF), 0);
        lv_obj_align(sym, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_t *lbl = lv_label_create(app->app_btn);
        lv_label_set_text(lbl, name);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xE6ECF5), 0);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
        int idx = (int)(app - s_device_apps);
        lv_obj_add_event_cb(app->app_btn, [](lv_event_t *e){
            int i = (int)(intptr_t)lv_event_get_user_data(e);
            switch_to_device(i);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    }

    app->active = true;
    s_device_app_count++;
    xSemaphoreGive(s_device_app_mutex);
    ESP_LOGI("UI", "Added device app: %s", name);
    return app;
}

/* ── Delete a device app: remove button, SD entry, unpair from C6 ── */
static void device_app_delete(int idx) {
    if (idx < 0 || idx >= MAX_DEVICE_APPS || !s_device_apps[idx].active) return;
    if (!s_device_app_mutex || xSemaphoreTake(s_device_app_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    const char *name = s_device_apps[idx].name;
    ESP_LOGI("UI", "Deleting device app: %s (idx=%d)", name, idx);

    /* Remove sidebar button */
    if (s_device_apps[idx].app_btn) {
        lv_obj_delete(s_device_apps[idx].app_btn);
        s_device_apps[idx].app_btn = NULL;
    }

    /* Send CANCEL:<name> to C6 to unpair */
    {
        char cancel_buf[64];
        snprintf(cancel_buf, sizeof(cancel_buf), "CANCEL:%s", name);
        uart_receiver_send(cancel_buf);
    }

    /* Remove from SD card scanned.txt */
    if (sd_card_bg_is_ready() || sd_card_bg_init() == ESP_OK) {
        FILE *f = fopen(SCANNED_FILE, "r");
        if (f) {
            char lines[MAX_SCANNED_DEVICES][64];
            int count = 0;
            char line[64];
            while (fgets(line, sizeof(line), f)) {
                size_t len = strlen(line);
                while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
                if (len > 0 && strcmp(line, name) != 0 && count < MAX_SCANNED_DEVICES) {
                    strncpy(lines[count], line, sizeof(lines[0]) - 1);
                    lines[count][sizeof(lines[0]) - 1] = '\0';
                    count++;
                }
            }
            fclose(f);
            f = fopen(SCANNED_FILE, "w");
            if (f) {
                for (int i = 0; i < count; i++) {
                    fprintf(f, "%s\n", lines[i]);
                }
                fclose(f);
            }
        }
    }

    /* Shift remaining entries to fill the gap */
    for (int i = idx + 1; i < MAX_DEVICE_APPS; i++) {
        if (!s_device_apps[i].active) break;
        s_device_apps[i - 1] = s_device_apps[i];
    }
    s_device_apps[s_device_app_count - 1].active = false;
    s_device_apps[s_device_app_count - 1].name[0] = '\0';
    s_device_apps[s_device_app_count - 1].app_btn = NULL;
    s_device_app_count--;

    xSemaphoreGive(s_device_app_mutex);

    /* If currently viewing this device, go back to main screen */
    if (s_current_device_idx == idx) {
        lv_screen_load(scr_main);
        s_current_device_idx = -1;
    } else if (s_current_device_idx > idx) {
        s_current_device_idx--;
    }
}

/* Copy data into udp_node_data_t (shared helper) */
static void copy_sensor_data(udp_node_data_t *dst, const ble_sensor_data_t *src) {
    if (!dst || !src) return;
    dst->valid = true;
    if (strcmp(src->sensor_class, "fog_status") == 0) {
        dst->fog = src->fog; dst->fog_auto = src->fog_auto; dst->fog_remain = src->fog_remain;
    } else if (strcmp(src->sensor_class, "fan_status") == 0) {
        dst->fan = src->fan; dst->fan_auto = src->fan_auto; dst->fan_remain = src->fan_remain;
    } else if (strcmp(src->sensor_class, "lid_status") == 0) {
        dst->lid = src->lid; dst->lid_auto = src->lid_auto; dst->lid_remain = src->lid_remain;
    } else if (strcmp(src->sensor_class, "status_only") == 0) {
        // Pure status update: ONLY update actuator control fields, do NOT overwrite telemetry!
        dst->uv = src->uv; dst->uv_auto = src->uv_auto; dst->uv_remain = src->uv_remain; dst->uv_dur = src->uv_dur;
        dst->fog = src->fog; dst->fog_auto = src->fog_auto; dst->fog_remain = src->fog_remain; dst->fog_dur = src->fog_dur;
        dst->fan = src->fan; dst->fan_auto = src->fan_auto; dst->fan_remain = src->fan_remain; dst->fan_dur = src->fan_dur;
        dst->lid = src->lid; dst->lid_auto = src->lid_auto; dst->lid_remain = src->lid_remain; dst->lid_dur = src->lid_dur;
    } else {
        dst->temp = src->temp; dst->hum = src->hum; dst->odor = src->odor;
        dst->hcho = src->hcho; dst->co = src->co; dst->voc = src->voc;
        dst->co2 = src->co2; dst->pred = src->pred; dst->conf = src->conf; dst->fresh = src->fresh;
        dst->uv = src->uv; dst->uv_auto = src->uv_auto; dst->uv_remain = src->uv_remain; dst->uv_dur = src->uv_dur;
        dst->fog = src->fog; dst->fog_auto = src->fog_auto; dst->fog_remain = src->fog_remain; dst->fog_dur = src->fog_dur;
        dst->fan = src->fan; dst->fan_auto = src->fan_auto; dst->fan_remain = src->fan_remain; dst->fan_dur = src->fan_dur;
        dst->lid = src->lid; dst->lid_auto = src->lid_auto; dst->lid_remain = src->lid_remain; dst->lid_dur = src->lid_dur;
        strncpy(dst->sensor_class, src->sensor_class, sizeof(dst->sensor_class) - 1);
    }
}

/* Update data for an existing device app (created by QR scan, not auto-created) */
static void device_app_update_existing(const char *name, const ble_sensor_data_t *data) {
    if (!name || !data || !s_device_app_mutex) return;
    if (xSemaphoreTake(s_device_app_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    for (int i = 0; i < MAX_DEVICE_APPS; i++) {
        if (s_device_apps[i].active && strcmp(s_device_apps[i].name, name) == 0) {
            copy_sensor_data(&s_device_apps[i].data, data);
            break;
        }
    }
    xSemaphoreGive(s_device_app_mutex);
}

void ui_handle_sensor_data(const ble_sensor_data_t *data) {
    if (!data || !s_udp_data_mutex) return;

    bool current_uv = false;
    bool current_fog = false;
    bool current_fan = false;
    bool current_lid = false;

    if (xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        const char *dev_name = data->node;
        if (!dev_name || !dev_name[0]) dev_name = "Unknown";

        /* Only update existing apps (created by QR scan), never auto-create */
        device_app_update_existing(dev_name, data);

        /* Legacy structs for dashboard + control compatibility */
        udp_node_data_t *target = NULL;
        if (strcmp(dev_name, "S3_Receiver") == 0 || strcmp(dev_name, "S3_01") == 0 ||
            strcmp(dev_name, "S3_A") == 0 || strcmp(dev_name, "S3A") == 0 || strcmp(dev_name, "Node S3-A") == 0) {
            target = &s_udp_node1;
        } else if (strcmp(dev_name, "S3_03") == 0 || strcmp(dev_name, "S3_B") == 0 ||
                   strcmp(dev_name, "S3B") == 0 || strcmp(dev_name, "Node S3-B") == 0) {
            target = &s_udp_node2;
        }
        if (target) copy_sensor_data(target, data);

        // Fetch the fully merged and current state of the CURRENTLY ACTIVE device from persistent storage
        if (s_current_device_idx >= 0 && s_current_device_idx < MAX_DEVICE_APPS) {
            device_app_t *a = &s_device_apps[s_current_device_idx];
            if (a->active) {
                current_uv = a->data.uv;
                current_fog = a->data.fog;
                current_fan = a->data.fan;
                current_lid = a->data.lid;
            }
        } else {
            current_uv = s_udp_node1.uv;
            current_fog = s_udp_node1.fog;
            current_fan = s_udp_node1.fan;
            current_lid = s_udp_node1.lid;
        }

        xSemaphoreGive(s_udp_data_mutex);
    }

    // Call async updates with the combined, correct state of the active node
    lv_async_call(uv_update_ui_cb, (void *)(intptr_t)current_uv);
    lv_async_call(fog_update_ui_cb, (void *)(intptr_t)current_fog);
    lv_async_call(fan_update_ui_cb, (void *)(intptr_t)current_fan);
    lv_async_call(lid_update_ui_cb, (void *)(intptr_t)current_lid);
}

/* Restore sidebar apps from paired devices saved in NVS (called once at boot) */
void ui_restore_paired_apps(void) {
    /* Restore all historically scanned devices from SD card */
    restore_scanned_sd();
    /* Also restore active C6 pairing from NVS pairing list */
    int pc = uart_receiver_get_paired_count();
    for (int i = 0; i < pc; i++) {
        char name[32];
        if (uart_receiver_get_paired_name_by_index(i, name, sizeof(name))) {
            device_app_ensure(name);
        }
    }
}

void ui_get_node1_data(ui_sensor_data_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        out->valid = s_udp_node1.valid;
        out->temp = s_udp_node1.temp;
        out->hum = s_udp_node1.hum;
        out->odor = s_udp_node1.odor;
        out->hcho = s_udp_node1.hcho;
        out->co = s_udp_node1.co;
        out->voc = s_udp_node1.voc;
        out->co2 = s_udp_node1.co2;
        out->pred = s_udp_node1.pred;
        strncpy(out->sensor_class, s_udp_node1.sensor_class, sizeof(out->sensor_class) - 1);
        out->conf = s_udp_node1.conf;
        out->fresh = s_udp_node1.fresh;
        out->uv = s_udp_node1.uv;
        out->fog = s_udp_node1.fog;
        out->fan = s_udp_node1.fan;
        out->lid = s_udp_node1.lid;
        xSemaphoreGive(s_udp_data_mutex);
    }
}

void ui_set_uv(bool on)
{
    if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_udp_node1.uv = on;
        int di = s_current_device_idx;
        if (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) {
            s_device_apps[di].data.uv = on;
        }
        xSemaphoreGive(s_udp_data_mutex);
    }
    lv_async_call(uv_update_ui_cb, (void *)(intptr_t)on);
}

void ui_set_fog(bool on)
{
    if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_udp_node1.fog = on;
        int di = s_current_device_idx;
        if (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) {
            s_device_apps[di].data.fog = on;
        }
        xSemaphoreGive(s_udp_data_mutex);
    }
    lv_async_call(fog_update_ui_cb, (void *)(intptr_t)on);
}

void ui_set_fan(bool on)
{
    if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_udp_node1.fan = on;
        int di = s_current_device_idx;
        if (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) {
            s_device_apps[di].data.fan = on;
        }
        xSemaphoreGive(s_udp_data_mutex);
    }
    lv_async_call(fan_update_ui_cb, (void *)(intptr_t)on);
}

void ui_set_lid(bool on)
{
    if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_udp_node1.lid = on;
        int di = s_current_device_idx;
        if (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) {
            s_device_apps[di].data.lid = on;
        }
        xSemaphoreGive(s_udp_data_mutex);
    }
    lv_async_call(lid_update_ui_cb, (void *)(intptr_t)on);
}

void ui_handle_warmup(int remaining)
{
    LV_UNUSED(remaining);
    /* Warmup label is updated in fake_data_timer_cb via uart_receiver_get_warmup_remaining() */
}

void ui_send_c6_command(const char *cmd)
{
    uart_receiver_send(cmd);
}



void ui_get_node2_data(ui_sensor_data_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        out->valid = s_udp_node2.valid;
        out->temp = s_udp_node2.temp;
        out->hum = s_udp_node2.hum;
        out->odor = s_udp_node2.odor;
        out->hcho = s_udp_node2.hcho;
        out->co = s_udp_node2.co;
        out->voc = s_udp_node2.voc;
        out->co2 = s_udp_node2.co2;
        out->pred = s_udp_node2.pred;
        strncpy(out->sensor_class, s_udp_node2.sensor_class, sizeof(out->sensor_class) - 1);
        out->conf = s_udp_node2.conf;
        out->fresh = s_udp_node2.fresh;
        out->uv = s_udp_node2.uv;
        out->fog = s_udp_node2.fog;
        out->fan = s_udp_node2.fan;
        xSemaphoreGive(s_udp_data_mutex);
    }
}

const char* ui_get_ai_result(void) {
    return s_ai_result_buffer;
}

/* ========================================================================
 * DASHBOARD UI
 * ======================================================================== */

static lv_obj_t * vol_slider;
static lv_obj_t * vol_label;
static lv_obj_t * btn_qr_entry;

static int current_volume = 80;

static const lv_color_t COLOR_BG = lv_color_hex(0x0B1020);
static const lv_color_t COLOR_PANEL = lv_color_hex(0x151C30);
static const lv_color_t COLOR_PANEL_ALT = lv_color_hex(0x10182A);
static const lv_color_t COLOR_BORDER = lv_color_hex(0x26324A);
static const lv_color_t COLOR_TEXT = lv_color_hex(0xE6ECF5);
static const lv_color_t COLOR_MUTED = lv_color_hex(0x91A0B8);
static const lv_color_t COLOR_ACCENT = lv_color_hex(0x4AA3FF);
static const lv_color_t COLOR_SUCCESS = lv_color_hex(0x39D98A);
static const lv_color_t COLOR_WARNING = lv_color_hex(0xFFB84D);

enum ModelSource {
    SRC_S3,
    SRC_SD,
    SRC_NEW
};

struct UnifiedModelItem {
    std::string name;
    int size;
    ModelSource source;
    int s3_idx; // 如果是 S3 模型，其在 S3 列表中的索引
    bool active; // 是否是当前活跃模型
    std::string status_text; // 用于描述特殊状态
};

// 静态全局统一模型列表容器，有效规避野指针崩溃
static std::vector<UnifiedModelItem> s_unified_list;

static void open_model_switch_modal_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    if (model_switch_modal) {
        lv_obj_delete(model_switch_modal);
        model_switch_modal = NULL;
    }

    model_switch_modal = lv_obj_create(scr_main);
    apply_no_shadow(model_switch_modal);
    lv_obj_set_size(model_switch_modal, 600, 420); // 宽 450->600, 高 300->420
    lv_obj_center(model_switch_modal);
    lv_obj_set_style_bg_color(model_switch_modal, lv_color_hex(0x151C30), 0);
    lv_obj_set_style_bg_opa(model_switch_modal, 242, 0);
    lv_obj_set_style_border_color(model_switch_modal, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(model_switch_modal, 2, 0);
    lv_obj_set_style_radius(model_switch_modal, 20, 0);
    lv_obj_clear_flag(model_switch_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(model_switch_modal);
    lv_label_set_text(title, "Select AI Model");
    lv_obj_set_style_text_color(title, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    lv_obj_t * list_cont = lv_obj_create(model_switch_modal);
    apply_no_shadow(list_cont);
    lv_obj_set_size(list_cont, 540, 280); // 宽 390->540, 高 170->280
    lv_obj_align(list_cont, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(list_cont, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    // 启用垂直滑动和滚动条，方便大量模型滑动交互
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);
    lv_obj_add_flag(list_cont, LV_OBJ_FLAG_SCROLLABLE);

    // ─── 统一模型聚合分组提取 ───
    s_unified_list.clear();

    // 1. 提取动态新接收 / 传输中的模型 (SRC_NEW)
    std::vector<UnifiedModelItem> incoming_list;
    ModelMgrStateInfo state_info;
    model_mgr_get_state(&state_info);
    if (state_info.state != MCS_IDLE && strlen(state_info.name) > 0) {
        UnifiedModelItem item;
        item.name = state_info.name;
        item.size = state_info.total_size;
        item.source = SRC_NEW;
        item.s3_idx = -1;
        item.active = false;
        if (state_info.state == MCS_DOWNLOADING) {
            item.status_text = " [Cloud Downloading...]";
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), " [Syncing to S3... %d%%]", state_info.progress_percent);
            item.status_text = buf;
        }
        incoming_list.push_back(item);
    }

    // 2. 提取 C6/S3 接收节点端的在片模型列表 (SRC_S3)
    std::vector<UnifiedModelItem> s3_list;
    int s3_count = uart_receiver_get_model_count();
    for (int i = 0; i < s3_count; i++) {
        const model_item_t *m = uart_receiver_get_model(i);
        if (!m) continue;
        
        UnifiedModelItem item;
        item.name = m->name;
        item.size = m->size;
        item.source = SRC_S3;
        item.s3_idx = i;
        item.active = m->active;
        item.status_text = m->active ? " [Active]" : "";
        s3_list.push_back(item);
    }

    // 3. 提取本地 SD 卡物理存储的模型列表 (SRC_SD)
    std::vector<UnifiedModelItem> sd_list;
    if (sd_card_bg_is_ready()) {
        DIR *d = opendir(CONFIG_BSP_SD_MOUNT_POINT "/model");
        if (d) {
            struct dirent *dir;
            while ((dir = readdir(d)) != NULL) {
                std::string name = dir->d_name;
                if (name == "." || name == "..") continue;
                
                char sub_path[256];
                snprintf(sub_path, sizeof(sub_path), CONFIG_BSP_SD_MOUNT_POINT "/model/%s", dir->d_name);
                
                struct stat st;
                if (stat(sub_path, &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        // Subdirectory: look for <name>.tflite inside `/model/<name>/`
                        char tflite_path[512];
                        snprintf(tflite_path, sizeof(tflite_path), "%s/%s.tflite", sub_path, name.c_str());
                        struct stat st_file;
                        if (stat(tflite_path, &st_file) == 0 && S_ISREG(st_file.st_mode)) {
                            UnifiedModelItem item;
                            item.name = name;
                            item.size = st_file.st_size;
                            item.source = SRC_SD;
                            item.s3_idx = -1;
                            item.active = false;
                            item.status_text = " [SD Stored]";
                            sd_list.push_back(item);
                        }
                    } else if (S_ISREG(st.st_mode)) {
                        // Flat file backward compatibility
                        std::string fname = name;
                        if (fname.size() > 7 && fname.compare(fname.size() - 7, 7, ".tflite") == 0) {
                            std::string bname = fname.substr(0, fname.size() - 7);
                            UnifiedModelItem item;
                            item.name = bname;
                            item.size = st.st_size;
                            item.source = SRC_SD;
                            item.s3_idx = -1;
                            item.active = false;
                            item.status_text = " [SD Stored]";
                            sd_list.push_back(item);
                        }
                    }
                }
            }
            closedir(d);
        }
    }

    // ─── 绘制三路模型分组视口 (Grouped Scrolling View) ───

    // A. 绘制“新接收 / 下载中”分组
    if (!incoming_list.empty()) {
        lv_obj_t * sec_lbl = lv_label_create(list_cont);
        lv_label_set_text(sec_lbl, "⚡ INCOMING & OTA");
        lv_obj_set_style_text_color(sec_lbl, COLOR_WARNING, 0);
        lv_obj_set_style_text_font(sec_lbl, &lv_font_montserrat_12, 0);
        
        for (const auto &item : incoming_list) {
            s_unified_list.push_back(item);
            int global_idx = s_unified_list.size() - 1;

            char item_text[128];
            snprintf(item_text, sizeof(item_text), "%s (Size: %d KB)%s",
                     item.name.c_str(), item.size / 1024, item.status_text.c_str());

            lv_obj_t * btn = lv_btn_create(list_cont);
            lv_obj_set_width(btn, lv_pct(100));
            lv_obj_set_height(btn, 36);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x2D1A3F), 0); // 尊贵紫金
            lv_obj_set_style_radius(btn, 8, 0);
            
            lv_obj_t * lbl = lv_label_create(btn);
            lv_label_set_text(lbl, item_text);
            lv_obj_set_style_text_color(lbl, COLOR_WARNING, 0);
            lv_obj_center(lbl);

            lv_obj_add_event_cb(btn, [](lv_event_t * ev) {
                int idx = (int)(intptr_t)lv_event_get_user_data(ev);
                if (idx >= 0 && idx < (int)s_unified_list.size()) {
                    const auto &target = s_unified_list[idx];
                    ESP_LOGI("UI", "Model '%s' is in progress, ignore.", target.name.c_str());
                }
            }, LV_EVENT_CLICKED, (void *)(intptr_t)global_idx);
        }
    }

    // B. 绘制“S3 上部署的模型”分组
    {
        lv_obj_t * sec_lbl = lv_label_create(list_cont);
        lv_label_set_text(sec_lbl, "🟢 DEPLOYED ON S3 RECEIVER");
        lv_obj_set_style_text_color(sec_lbl, COLOR_SUCCESS, 0);
        lv_obj_set_style_text_font(sec_lbl, &lv_font_montserrat_12, 0);

        if (s3_list.empty()) {
            lv_obj_t * empty_lbl = lv_label_create(list_cont);
            lv_label_set_text(empty_lbl, "  No models active on S3. Select below to flash.");
            lv_obj_set_style_text_color(empty_lbl, COLOR_MUTED, 0);
            lv_obj_set_style_text_font(empty_lbl, &lv_font_source_han_sans_sc_14_cjk, 0);
        } else {
            for (const auto &item : s3_list) {
                s_unified_list.push_back(item);
                int global_idx = s_unified_list.size() - 1;

                char item_text[128];
                snprintf(item_text, sizeof(item_text), "%s (Size: %d KB)%s",
                         item.name.c_str(), item.size / 1024, item.status_text.c_str());

                // 创建 Flex 行容器作为条目背景，以并排承载切换按钮与删除按钮
                lv_obj_t * item_row = lv_obj_create(list_cont);
                apply_no_shadow(item_row);
                lv_obj_set_size(item_row, lv_pct(100), 40);
                lv_obj_set_style_bg_opa(item_row, LV_OPA_TRANSP, 0); // 容器完全透明，不显杂色
                lv_obj_set_style_border_width(item_row, 0, 0);
                lv_obj_set_style_pad_all(item_row, 0, 0);
                lv_obj_set_flex_flow(item_row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(item_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                lv_obj_clear_flag(item_row, LV_OBJ_FLAG_SCROLLABLE);

                // 左侧切换按钮
                lv_obj_t * btn = lv_btn_create(item_row);
                lv_obj_set_width(btn, item.active ? lv_pct(100) : lv_pct(83)); // 活跃占满，非活跃缩减以留出空位给删除按钮
                lv_obj_set_height(btn, 36);
                lv_obj_set_style_bg_color(btn, item.active ? lv_color_hex(0x192543) : lv_color_hex(0x0B1020), 0); // 活跃灰蓝，闲置暗底
                lv_obj_set_style_radius(btn, 8, 0);

                lv_obj_t * lbl = lv_label_create(btn);
                lv_label_set_text(lbl, item_text);
                lv_obj_set_style_text_color(lbl, COLOR_TEXT, 0);
                lv_obj_center(lbl);

                lv_obj_add_event_cb(btn, [](lv_event_t * ev) {
                    int idx = (int)(intptr_t)lv_event_get_user_data(ev);
                    if (idx >= 0 && idx < (int)s_unified_list.size()) {
                        const auto &target = s_unified_list[idx];
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "model load %d", target.s3_idx);
                        
                        char dn[32] = "";
                        if (!uart_receiver_get_first_paired_name(dn, sizeof(dn)) || strlen(dn) == 0) {
                            int di = s_current_device_idx;
                            const char *app_dn = (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) ? s_device_apps[di].name : "";
                            if (strlen(app_dn) > 0) {
                                strncpy(dn, app_dn, sizeof(dn) - 1);
                                dn[sizeof(dn) - 1] = '\0';
                            } else {
                                strncpy(dn, "S3_03", sizeof(dn) - 1);
                                dn[sizeof(dn) - 1] = '\0';
                            }
                        }
                        
                        uart_receiver_send_cmd(dn, cmd);
                        if (main_model_name_label) lv_label_set_text(main_model_name_label, "Model: Switching...");
                        if (main_model_status_led) lv_obj_set_style_bg_color(main_model_status_led, lv_color_hex(0xFFB84D), 0);
                    }
                    close_modal_with_zoom_anim_delete(&model_switch_modal);
                }, LV_EVENT_CLICKED, (void *)(intptr_t)global_idx);

                // 右侧红色的删除按钮（仅当非活跃模型时展示，防热删除运行中模型的安全保护机制）
                if (!item.active) {
                    lv_obj_t * del_btn = lv_btn_create(item_row);
                    lv_obj_set_size(del_btn, lv_pct(14), 36);
                    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0xE53E3E), 0); // 警示高亮红
                    lv_obj_set_style_radius(del_btn, 8, 0);

                    lv_obj_t * del_lbl = lv_label_create(del_btn);
                    lv_label_set_text(del_lbl, "X");
                    lv_obj_set_style_text_color(del_lbl, lv_color_hex(0xFFFFFF), 0);
                    lv_obj_center(del_lbl);

                    lv_obj_add_event_cb(del_btn, [](lv_event_t * ev) {
                        int idx = (int)(intptr_t)lv_event_get_user_data(ev);
                        if (idx >= 0 && idx < (int)s_unified_list.size()) {
                            const auto &target = s_unified_list[idx];
                            ESP_LOGW("UI", "User triggered remote model delete for S3 index %d (%s)", target.s3_idx, target.name.c_str());
                            
                            // 向下位机发送串口删除指令
                            char cmd[64];
                            snprintf(cmd, sizeof(cmd), "model delete %d", target.s3_idx);
                            
                            char dn[32] = "";
                            if (!uart_receiver_get_first_paired_name(dn, sizeof(dn)) || strlen(dn) == 0) {
                                int di = s_current_device_idx;
                                const char *app_dn = (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) ? s_device_apps[di].name : "";
                                if (strlen(app_dn) > 0) {
                                    strncpy(dn, app_dn, sizeof(dn) - 1);
                                    dn[sizeof(dn) - 1] = '\0';
                                } else {
                                    strncpy(dn, "S3_03", sizeof(dn) - 1);
                                    dn[sizeof(dn) - 1] = '\0';
                                }
                            }
                            
                            uart_receiver_send_cmd(dn, cmd);

                            // 弹出提示框，给用户即时删除反馈
                            ui_show_ai_analysis("Model deletion request sent to S3 node.");
                        }
                        close_modal_with_zoom_anim_delete(&model_switch_modal);
                    }, LV_EVENT_CLICKED, (void *)(intptr_t)global_idx);
                }
            }
        }
    }

    // C. 绘制“本地 SD 卡模型”分组
    {
        lv_obj_t * sec_lbl = lv_label_create(list_cont);
        lv_label_set_text(sec_lbl, "💾 STORED IN LOCAL SD CARD (CLICK TO FLASH)");
        lv_obj_set_style_text_color(sec_lbl, COLOR_ACCENT, 0);
        lv_obj_set_style_text_font(sec_lbl, &lv_font_montserrat_12, 0);

        if (sd_list.empty()) {
            lv_obj_t * empty_lbl = lv_label_create(list_cont);
            lv_label_set_text(empty_lbl, "  No model files in SD card /model folder");
            lv_obj_set_style_text_color(empty_lbl, COLOR_MUTED, 0);
            lv_obj_set_style_text_font(empty_lbl, &lv_font_source_han_sans_sc_14_cjk, 0);
        } else {
            for (const auto &item : sd_list) {
                s_unified_list.push_back(item);
                int global_idx = s_unified_list.size() - 1;

                char item_text[128];
                snprintf(item_text, sizeof(item_text), "%s (Size: %d KB)%s",
                         item.name.c_str(), item.size / 1024, item.status_text.c_str());

                // 创建行容器作为条目背景，以承载烧录选择按钮与删除按钮
                lv_obj_t * item_row = lv_obj_create(list_cont);
                apply_no_shadow(item_row);
                lv_obj_set_size(item_row, lv_pct(100), 40);
                lv_obj_set_style_bg_opa(item_row, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(item_row, 0, 0);
                lv_obj_set_style_pad_all(item_row, 0, 0);
                lv_obj_set_flex_flow(item_row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(item_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                lv_obj_clear_flag(item_row, LV_OBJ_FLAG_SCROLLABLE);

                // 左侧烧录选择按钮 (占 83%)
                lv_obj_t * btn = lv_btn_create(item_row);
                lv_obj_set_width(btn, lv_pct(83));
                lv_obj_set_height(btn, 36);
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x0E1A2F), 0); // 本地 SD 为淡雅藏蓝
                lv_obj_set_style_radius(btn, 8, 0);

                lv_obj_t * lbl = lv_label_create(btn);
                lv_label_set_text(lbl, item_text);
                lv_obj_set_style_text_color(lbl, COLOR_TEXT, 0);
                lv_obj_center(lbl);

                lv_obj_add_event_cb(btn, [](lv_event_t * ev) {
                    int idx = (int)(intptr_t)lv_event_get_user_data(ev);
                    if (idx >= 0 && idx < (int)s_unified_list.size()) {
                        const auto &target = s_unified_list[idx];
                        extern bool model_mgr_load_and_send_sd_model(const char *model_name);
                        ESP_LOGI("UI", "Flashing local SD model '%s' directly to S3 Receiver...", target.name.c_str());
                        if (model_mgr_load_and_send_sd_model(target.name.c_str())) {
                            if (main_model_name_label) lv_label_set_text(main_model_name_label, "Model: Syncing SD...");
                            if (main_model_status_led) lv_obj_set_style_bg_color(main_model_status_led, lv_color_hex(0xFFB84D), 0);
                        }
                    }
                    close_modal_with_zoom_anim_delete(&model_switch_modal);
                }, LV_EVENT_CLICKED, (void *)(intptr_t)global_idx);

                // 右侧红色的删除本地模型文件按钮 (占 14%)
                lv_obj_t * del_btn = lv_btn_create(item_row);
                lv_obj_set_size(del_btn, lv_pct(14), 36);
                lv_obj_set_style_bg_color(del_btn, lv_color_hex(0xE53E3E), 0); // 警示高亮红
                lv_obj_set_style_radius(del_btn, 8, 0);

                lv_obj_t * del_lbl = lv_label_create(del_btn);
                lv_label_set_text(del_lbl, "X");
                lv_obj_set_style_text_color(del_lbl, lv_color_hex(0xFFFFFF), 0);
                lv_obj_center(del_lbl);

                lv_obj_add_event_cb(del_btn, [](lv_event_t * ev) {
                    int idx = (int)(intptr_t)lv_event_get_user_data(ev);
                    if (idx >= 0 && idx < (int)s_unified_list.size()) {
                        const auto &target = s_unified_list[idx];
                        extern bool model_mgr_delete_local_model(const char *model_name);
                        ESP_LOGW("UI", "Deleting local SD model: %s", target.name.c_str());
                        
                        if (model_mgr_delete_local_model(target.name.c_str())) {
                            ESP_LOGI("UI", "Successfully deleted local model: %s", target.name.c_str());
                            ui_show_ai_analysis("Local SD model deleted successfully.");
                        } else {
                            ESP_LOGE("UI", "Failed to delete local model: %s", target.name.c_str());
                            ui_show_ai_analysis("Failed to delete local SD model.");
                        }
                    }
                    // 删除后关闭当前模态弹窗，以刷新模型文件展示
                    close_modal_with_zoom_anim_delete(&model_switch_modal);
                }, LV_EVENT_CLICKED, (void *)(intptr_t)global_idx);
            }
        }
    }

    lv_obj_t * close_btn = lv_btn_create(model_switch_modal);
    lv_obj_set_size(close_btn, 100, 36);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_add_event_cb(close_btn, [](lv_event_t * ev) {
        LV_UNUSED(ev);
        close_modal_with_zoom_anim_delete(&model_switch_modal);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(close_lbl);
    open_modal_with_zoom_anim(model_switch_modal);
}

static void update_volume_ui(int vol, bool sync_sliders) {
    current_volume = vol;
    if (vol_label) {
        lv_label_set_text_fmt(vol_label, "%d%%", vol);
    }
    if (cc_volume_label) {
        lv_label_set_text_fmt(cc_volume_label, "%d%%", vol);
    }
    if (sync_sliders) {
        if (vol_slider) {
            lv_slider_set_value(vol_slider, vol, LV_ANIM_OFF);
        }
        if (cc_volume_slider) {
            lv_slider_set_value(cc_volume_slider, vol, LV_ANIM_OFF);
        }
    }
}

int ui_get_current_volume(void) {
    return current_volume;
}

void ui_sync_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    bsp_extra_codec_volume_set(volume, NULL);
    esp_err_t ret = esp_lv_adapter_lock(pdMS_TO_TICKS(100));
    if (ret == ESP_OK) {
        update_volume_ui(volume, true);
        esp_lv_adapter_unlock();
    }
}

static void set_wifi_status_text(const char * text, lv_color_t color) {
    if (wifi_status_label) {
        lv_label_set_text(wifi_status_label, text);
        lv_obj_set_style_text_color(wifi_status_label, color, 0);
    }
    if (cc_wifi_status_label) {
        const char * cc_text = text;
        if (strncmp(text, "WiFi: ", 6) == 0) {
            cc_text = text + 6;
        }
        lv_label_set_text(cc_wifi_status_label, cc_text);
        lv_obj_set_style_text_color(cc_wifi_status_label, color, 0);
    }
    // 动态 Wi-Fi 卡片发光边框
    if (wifi_tile) {
        bool active = (strstr(text, "Disconnected") == NULL);
        lv_obj_set_style_border_color(wifi_tile, active ? color : lv_color_hex(0x2A3A5A), 0);
        lv_obj_set_style_border_width(wifi_tile, active ? 2 : 1, 0);
    }
}

static void cc_set_y(void * obj, int32_t y) {
    lv_obj_set_y((lv_obj_t *)obj, y);
}

static void anim_cc_ready_cb(lv_anim_t * a) {
    cc_is_open = (lv_obj_get_y(cc_overlay) > -100);
}

static void close_control_center(void) {
    if (!cc_overlay || !cc_is_open) {
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cc_overlay);
    lv_anim_set_exec_cb(&a, cc_set_y);
    lv_anim_set_values(&a, lv_obj_get_y(cc_overlay), -420);
    lv_anim_set_time(&a, 350);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, anim_cc_ready_cb);
    lv_anim_start(&a);
}

static void open_control_center(void) {
    if (!cc_overlay || cc_is_open) {
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cc_overlay);
    lv_anim_set_exec_cb(&a, cc_set_y);
    lv_anim_set_values(&a, lv_obj_get_y(cc_overlay), 20);
    lv_anim_set_time(&a, 400);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_ready_cb(&a, anim_cc_ready_cb);
    lv_anim_start(&a);
}

static void cc_close_btn_cb(lv_event_t * e) {
    LV_UNUSED(e);
    close_control_center();
}

// Removed press animations to ensure stability

static void delayed_wifi_open_cb(lv_timer_t * t) {
    btn_open_wifi_cb(NULL);
    lv_timer_delete(t);
}

static void cc_wifi_tile_cb(lv_event_t * e) {
    LV_UNUSED(e);
    close_control_center();
    // Delay opening the Wi-Fi scan to avoid overlapping with the sliding animation,
    // which causes the UI thread to freeze.
    lv_timer_create(delayed_wifi_open_cb, 400, NULL);
}

static void cc_brightness_slider_cb(lv_event_t * e) {
    lv_obj_t * slider = (lv_obj_t *)lv_event_get_target(e);
    int brightness = (int)lv_slider_get_value(slider);
    bsp_display_brightness_set(brightness);
}

static void apps_drawer_anim_cb(void * obj, int32_t x) {
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void toggle_apps_drawer(bool open) {
    if (open == apps_drawer_open) return;
    apps_drawer_open = open;
    int32_t target_x = open ? 824 : 1024;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, apps_panel);
    lv_anim_set_exec_cb(&a, apps_drawer_anim_cb);
    lv_anim_set_values(&a, lv_obj_get_x(apps_panel), target_x);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static lv_point_t s_gesture_start_point = {0, 0};

static void main_screen_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_PRESSED) {
        lv_indev_t * indev = lv_indev_get_act();
        if (indev) {
            lv_indev_get_point(indev, &s_gesture_start_point);
        }
        return;
    }
    
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_BOTTOM) {
            // 顶端下滑手势（Y < 80）：触发展开下拉控制中心（ status bar 区域）
            if (s_gesture_start_point.y < 80) {
                open_control_center();
            }
        } else if (dir == LV_DIR_TOP) {
            // 任意区域上滑手势：收起/关闭控制中心
            close_control_center();
        } else if (dir == LV_DIR_LEFT) {
            // 右边缘向左滑手势（X > 880）：展开 APP 抽屉，防止和主界面左右拖拽冲突
            if (s_gesture_start_point.x > 880) {
                toggle_apps_drawer(true);
            }
        } else if (dir == LV_DIR_RIGHT) {
            // 任意区域向右滑手势：在抽屉打开时，收起抽屉
            if (apps_drawer_open) {
                toggle_apps_drawer(false);
            }
        }
    }
}

static void sub_screen_back_gesture_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_PRESSED) {
        lv_indev_t * indev = lv_indev_get_act();
        if (indev) {
            lv_indev_get_point(indev, &s_gesture_start_point);
        }
        return;
    }
    
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_RIGHT) {
            // 左边缘向右滑手势（X < 180）：全局右滑返回主界面，兼容高档手机侧滑返回体验
            if (s_gesture_start_point.x < 180) {
                ESP_LOGI("UI", "Sub-screen swipe right from edge, returning to main screen");
                lv_screen_load(scr_main);
            }
        }
    }
}

static void cc_upload_tile_cb(lv_event_t * e) {
    LV_UNUSED(e);
    bool now = !cloud_sync_is_enabled();
    cloud_sync_set_enabled(now);
    lv_obj_set_style_bg_color(sync_status_dot, now ? lv_color_hex(0x39D98A) : lv_color_hex(0x666666), 0);
    lv_label_set_text(sync_status_label, now ? "1s Sync" : "Off");
    lv_obj_set_style_text_color(sync_status_label, now ? lv_color_hex(0x39D98A) : lv_color_hex(0x91A0B8), 0);
    if (sync_tile) {
        lv_obj_set_style_border_color(sync_tile, now ? lv_color_hex(0xFFB84D) : lv_color_hex(0x2A3A5A), 0);
        lv_obj_set_style_border_width(sync_tile, now ? 2 : 1, 0);
    }
}

static void control_center_init(lv_obj_t * parent) {
    cc_overlay = lv_obj_create(parent);
    apply_no_shadow(cc_overlay);
    lv_obj_set_size(cc_overlay, 900, 350); // Slimmer panel height (350px)
    lv_obj_align(cc_overlay, LV_ALIGN_TOP_MID, 0, -420);
    lv_obj_set_style_bg_color(cc_overlay, lv_color_hex(0x0C1226), 0); // Deep navy background
    lv_obj_set_style_bg_opa(cc_overlay, LV_OPA_80, 0); // Frosted transparency
    lv_obj_set_style_border_color(cc_overlay, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(cc_overlay, 1, 0);
    lv_obj_set_style_radius(cc_overlay, 24, 0);
    lv_obj_clear_flag(cc_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Vibrant animated gradient border (iOS-style neon edge) */
    static lv_color_t s_cc_border_colors[6];
    s_cc_border_colors[0] = lv_color_hex(0x00F0FF); /* cyan */
    s_cc_border_colors[1] = lv_color_hex(0x4AA3FF); /* blue */
    s_cc_border_colors[2] = lv_color_hex(0xC084FC); /* purple */
    s_cc_border_colors[3] = lv_color_hex(0xFF6B9D); /* pink */
    s_cc_border_colors[4] = lv_color_hex(0x39D98A); /* green */
    s_cc_border_colors[5] = lv_color_hex(0xFFB84D); /* orange */
    lv_obj_set_style_border_color(cc_overlay, s_cc_border_colors[0], 0);
    lv_obj_set_style_border_opa(cc_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(cc_overlay, 2, 0);

    /* Animate the border color cycling through the vibrant palette */
    static int s_cc_color_idx = 0;
    lv_timer_create([](lv_timer_t * t){
        lv_obj_t * overlay = (lv_obj_t *)lv_timer_get_user_data(t);
        if (!overlay) return;
        s_cc_color_idx = (s_cc_color_idx + 1) % 6;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, overlay);
        lv_anim_set_exec_cb(&a, [](void * var, int32_t v){
            (void)v;
            lv_obj_set_style_border_color((lv_obj_t *)var, s_cc_border_colors[s_cc_color_idx], 0);
        });
        lv_anim_set_values(&a, 0, 1);
        lv_anim_set_duration(&a, 800);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }, 2500, cc_overlay);

    lv_obj_t * title = lv_label_create(cc_overlay);
    lv_label_set_text(title, "控制中心");
    lv_obj_set_style_text_font(title, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 18);

    lv_obj_t * handle = lv_obj_create(cc_overlay);
    apply_no_shadow(handle);
    lv_obj_set_size(handle, 100, 5); // Minimal drag handle
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0x596273), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_60, 0);
    lv_obj_set_style_radius(handle, 3, 0);
    lv_obj_clear_flag(handle, LV_OBJ_FLAG_SCROLLABLE);

    /* ===== Left Column (160px width cards) ===== */

    /* WiFi tile */
    wifi_tile = lv_obj_create(cc_overlay);
    apply_no_shadow(wifi_tile);
    lv_obj_set_size(wifi_tile, 160, 76);
    lv_obj_align(wifi_tile, LV_ALIGN_TOP_LEFT, 36, 56);
    lv_obj_set_style_bg_color(wifi_tile, lv_color_hex(0x1A2542), 0);
    lv_obj_set_style_bg_opa(wifi_tile, LV_OPA_50, 0);
    lv_obj_set_style_border_color(wifi_tile, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(wifi_tile, 1, 0);
    lv_obj_set_style_radius(wifi_tile, 16, 0);
    lv_obj_clear_flag(wifi_tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(wifi_tile, cc_wifi_tile_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * wifi_icon = lv_label_create(wifi_tile);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(wifi_icon, COLOR_ACCENT, 0);
    lv_obj_align(wifi_icon, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t * wifi_label_title = lv_label_create(wifi_tile);
    lv_label_set_text(wifi_label_title, "Wi-Fi");
    lv_obj_set_style_text_font(wifi_label_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wifi_label_title, COLOR_TEXT, 0);
    lv_obj_align(wifi_label_title, LV_ALIGN_TOP_LEFT, 46, 14);

    cc_wifi_status_label = lv_label_create(wifi_tile);
    lv_label_set_text(cc_wifi_status_label, "Disconnected");
    lv_obj_set_style_text_font(cc_wifi_status_label, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(cc_wifi_status_label, COLOR_MUTED, 0);
    lv_obj_align(cc_wifi_status_label, LV_ALIGN_BOTTOM_LEFT, 46, -14);

    /* C6 Bridge tile */
    c6_tile = lv_obj_create(cc_overlay);
    apply_no_shadow(c6_tile);
    lv_obj_set_size(c6_tile, 160, 76);
    lv_obj_align(c6_tile, LV_ALIGN_TOP_LEFT, 36, 142);
    lv_obj_set_style_bg_color(c6_tile, lv_color_hex(0x1A2542), 0);
    lv_obj_set_style_bg_opa(c6_tile, LV_OPA_50, 0);
    lv_obj_set_style_border_color(c6_tile, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(c6_tile, 1, 0);
    lv_obj_set_style_radius(c6_tile, 16, 0);
    lv_obj_clear_flag(c6_tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(c6_tile, [](lv_event_t * e) {
        ESP_LOGI("UI", "C6 Tile clicked");
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * c6_icon = lv_label_create(c6_tile);
    lv_label_set_text(c6_icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(c6_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(c6_icon, lv_color_hex(0x03DAC6), 0);
    lv_obj_align(c6_icon, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t * c6_label_title = lv_label_create(c6_tile);
    lv_label_set_text(c6_label_title, "C6 Bridge");
    lv_obj_set_style_text_font(c6_label_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(c6_label_title, COLOR_TEXT, 0);
    lv_obj_align(c6_label_title, LV_ALIGN_TOP_LEFT, 46, 14);

    c6_status_label = lv_label_create(c6_tile);
    lv_label_set_text(c6_status_label, "Connecting...");
    lv_obj_set_style_text_font(c6_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(c6_status_label, COLOR_MUTED, 0);
    lv_obj_align(c6_status_label, LV_ALIGN_BOTTOM_LEFT, 46, -14);

    c6_status_dot = lv_obj_create(c6_tile);
    apply_no_shadow(c6_status_dot);
    lv_obj_set_size(c6_status_dot, 8, 8);
    lv_obj_align(c6_status_dot, LV_ALIGN_BOTTOM_LEFT, 12, -14);
    lv_obj_set_style_bg_color(c6_status_dot, lv_color_hex(0x666666), 0);
    lv_obj_set_style_bg_opa(c6_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c6_status_dot, 4, 0);
    lv_obj_clear_flag(c6_status_dot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * c6_hint = lv_label_create(c6_tile);
    lv_label_set_text(c6_hint, "UART 115k");
    lv_obj_set_style_text_font(c6_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(c6_hint, lv_color_hex(0x4A5A7A), 0);
    lv_obj_align(c6_hint, LV_ALIGN_BOTTOM_LEFT, 24, -14);

    /* Cloud Sync toggle tile */
    sync_tile = lv_obj_create(cc_overlay);
    apply_no_shadow(sync_tile);
    lv_obj_set_size(sync_tile, 160, 76);
    lv_obj_align(sync_tile, LV_ALIGN_TOP_LEFT, 36, 228);
    lv_obj_set_style_bg_color(sync_tile, lv_color_hex(0x1A2542), 0);
    lv_obj_set_style_bg_opa(sync_tile, LV_OPA_50, 0);
    lv_obj_set_style_border_color(sync_tile, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(sync_tile, 1, 0);
    lv_obj_set_style_radius(sync_tile, 16, 0);
    lv_obj_clear_flag(sync_tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sync_tile, cc_upload_tile_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * sync_icon = lv_label_create(sync_tile);
    lv_label_set_text(sync_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(sync_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(sync_icon, lv_color_hex(0xFFB84D), 0);
    lv_obj_align(sync_icon, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t * sync_label_title = lv_label_create(sync_tile);
    lv_label_set_text(sync_label_title, "Cloud Sync");
    lv_obj_set_style_text_font(sync_label_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sync_label_title, COLOR_TEXT, 0);
    lv_obj_align(sync_label_title, LV_ALIGN_TOP_LEFT, 46, 14);

    sync_status_label = lv_label_create(sync_tile);
    lv_label_set_text(sync_status_label, "Off");
    lv_obj_set_style_text_font(sync_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sync_status_label, lv_color_hex(0x91A0B8), 0);
    lv_obj_align(sync_status_label, LV_ALIGN_BOTTOM_LEFT, 46, -14);

    sync_status_dot = lv_obj_create(sync_tile);
    apply_no_shadow(sync_status_dot);
    lv_obj_set_size(sync_status_dot, 8, 8);
    lv_obj_align(sync_status_dot, LV_ALIGN_BOTTOM_LEFT, 12, -14);
    lv_obj_set_style_bg_color(sync_status_dot, lv_color_hex(0x666666), 0);
    lv_obj_set_style_bg_opa(sync_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sync_status_dot, 4, 0);
    lv_obj_clear_flag(sync_status_dot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * sync_hint = lv_label_create(sync_tile);
    lv_label_set_text(sync_hint, "Sync Info");
    lv_obj_set_style_text_font(sync_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sync_hint, lv_color_hex(0x4A5A7A), 0);
    lv_obj_align(sync_hint, LV_ALIGN_BOTTOM_LEFT, 24, -14);

    /* ===== Right Column (620px width card sliders) ===== */

    /* Brightness tile */
    lv_obj_t * bright_tile = lv_obj_create(cc_overlay);
    apply_no_shadow(bright_tile);
    lv_obj_set_size(bright_tile, 620, 76);
    lv_obj_align(bright_tile, LV_ALIGN_TOP_LEFT, 244, 56);
    lv_obj_set_style_bg_color(bright_tile, lv_color_hex(0x141E3A), 0);
    lv_obj_set_style_bg_opa(bright_tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bright_tile, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(bright_tile, 1, 0);
    lv_obj_set_style_radius(bright_tile, 16, 0);
    lv_obj_clear_flag(bright_tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * bright_icon = lv_label_create(bright_tile);
    lv_label_set_text(bright_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(bright_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bright_icon, lv_color_hex(0xFFCC00), 0);
    lv_obj_align(bright_icon, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t * bright_label = lv_label_create(bright_tile);
    lv_label_set_text(bright_label, "Brightness");
    lv_obj_set_style_text_font(bright_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bright_label, COLOR_TEXT, 0);
    lv_obj_align(bright_label, LV_ALIGN_LEFT_MID, 48, 0);

    cc_brightness_slider = lv_slider_create(bright_tile);
    lv_obj_set_size(cc_brightness_slider, 440, 6);  // Thinned to 6px
    lv_obj_align(cc_brightness_slider, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_slider_set_range(cc_brightness_slider, 10, 100);
    lv_slider_set_value(cc_brightness_slider, 80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(cc_brightness_slider, lv_color_hex(0x192543), LV_PART_MAIN);
    lv_obj_set_style_radius(cc_brightness_slider, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cc_brightness_slider, lv_color_hex(0xFFB800), LV_PART_INDICATOR);
    lv_obj_set_style_radius(cc_brightness_slider, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cc_brightness_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_radius(cc_brightness_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_opa(cc_brightness_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(cc_brightness_slider, 4, LV_PART_KNOB); // Knob slightly larger than track
    lv_obj_add_event_cb(cc_brightness_slider, cc_brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Volume tile */
    lv_obj_t * volume_tile = lv_obj_create(cc_overlay);
    apply_no_shadow(volume_tile);
    lv_obj_set_size(volume_tile, 620, 76);
    lv_obj_align(volume_tile, LV_ALIGN_TOP_LEFT, 244, 142);
    lv_obj_set_style_bg_color(volume_tile, lv_color_hex(0x141E3A), 0);
    lv_obj_set_style_bg_opa(volume_tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(volume_tile, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(volume_tile, 1, 0);
    lv_obj_set_style_radius(volume_tile, 16, 0);
    lv_obj_clear_flag(volume_tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * volume_icon = lv_label_create(volume_tile);
    lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(volume_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(volume_icon, lv_color_hex(0x03DAC6), 0);
    lv_obj_align(volume_icon, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t * volume_label = lv_label_create(volume_tile);
    lv_label_set_text(volume_label, "Volume");
    lv_obj_set_style_text_font(volume_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(volume_label, COLOR_TEXT, 0);
    lv_obj_align(volume_label, LV_ALIGN_LEFT_MID, 48, 0);

    cc_volume_slider = lv_slider_create(volume_tile);
    lv_obj_set_size(cc_volume_slider, 380, 6);  // Thinned to 6px
    lv_obj_align(cc_volume_slider, LV_ALIGN_RIGHT_MID, -60, 0);
    lv_slider_set_range(cc_volume_slider, 0, 100);
    lv_obj_set_style_bg_color(cc_volume_slider, lv_color_hex(0x192543), LV_PART_MAIN);
    lv_obj_set_style_radius(cc_volume_slider, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cc_volume_slider, lv_color_hex(0x03DAC6), LV_PART_INDICATOR);
    lv_obj_set_style_radius(cc_volume_slider, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cc_volume_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_radius(cc_volume_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_opa(cc_volume_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(cc_volume_slider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(cc_volume_slider, vol_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    cc_volume_label = lv_label_create(volume_tile);
    lv_label_set_text(cc_volume_label, "80%");
    lv_obj_set_style_text_font(cc_volume_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cc_volume_label, lv_color_hex(0x03DAC6), 0);
    lv_obj_align(cc_volume_label, LV_ALIGN_RIGHT_MID, -16, 0);

    /* Wallpaper tile */
    lv_obj_t * wallpaper_tile = lv_obj_create(cc_overlay);
    apply_no_shadow(wallpaper_tile);
    lv_obj_set_size(wallpaper_tile, 620, 76);
    lv_obj_align(wallpaper_tile, LV_ALIGN_TOP_LEFT, 244, 228);
    lv_obj_set_style_bg_color(wallpaper_tile, lv_color_hex(0x141E3A), 0);
    lv_obj_set_style_bg_opa(wallpaper_tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(wallpaper_tile, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(wallpaper_tile, 1, 0);
    lv_obj_set_style_radius(wallpaper_tile, 16, 0);
    lv_obj_clear_flag(wallpaper_tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(wallpaper_tile, [](lv_event_t * e) {
        s_wallpaper_target = WALLPAPER_TARGET_MAIN;
        open_wallpaper_list();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * wp_icon = lv_label_create(wallpaper_tile);
    lv_label_set_text(wp_icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(wp_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(wp_icon, lv_color_hex(0xC084FC), 0);
    lv_obj_align(wp_icon, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t * wp_label = lv_label_create(wallpaper_tile);
    lv_label_set_text(wp_label, "Wallpaper");
    lv_obj_set_style_text_font(wp_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wp_label, COLOR_TEXT, 0);
    lv_obj_align(wp_label, LV_ALIGN_LEFT_MID, 48, -12);

    lv_obj_t * wp_hint = lv_label_create(wallpaper_tile);
    lv_label_set_text(wp_hint, "Tap to change main screen wallpaper");
    lv_obj_set_style_text_font(wp_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wp_hint, COLOR_MUTED, 0);
    lv_obj_align(wp_hint, LV_ALIGN_LEFT_MID, 48, 12);

    /* Footer instructions */
    lv_obj_t * footer = lv_label_create(cc_overlay);
    lv_label_set_text(footer, "上滑关闭控制中心");
    lv_obj_set_style_text_font(footer, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(0x4A5A7A), 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -10);

    update_volume_ui(current_volume, true);
    set_wifi_status_text("WiFi: Disconnected", lv_color_hex(0xAAAAAA));
}

static void ui_setup_premium_click_effect(lv_obj_t * obj, lv_color_t active_border_color, lv_color_t glow_color)
{
    // Enable scale/shadow transitions on state changes (LVGL v9 style)
    static const lv_style_prop_t transition_props[] = {
        LV_STYLE_BG_COLOR,
        LV_STYLE_BORDER_COLOR,
        LV_STYLE_TRANSFORM_SCALE_X,
        LV_STYLE_TRANSFORM_SCALE_Y,
        LV_STYLE_SHADOW_WIDTH,
        LV_STYLE_SHADOW_COLOR,
        (lv_style_prop_t)0
    };
    
    static lv_style_transition_dsc_t transition_dsc;
    static bool transition_dsc_inited = false;
    if (!transition_dsc_inited) {
        lv_style_transition_dsc_init(&transition_dsc, transition_props, lv_anim_path_ease_out, 120, 0, NULL);
        transition_dsc_inited = true;
    }

    // Apply normal state styles
    lv_obj_set_style_transform_scale_x(obj, 256, 0); // 100% scale (no zoom)
    lv_obj_set_style_transform_scale_y(obj, 256, 0);
    lv_obj_set_style_transition(obj, &transition_dsc, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);

    // Apply pressed state styles
    lv_obj_set_style_transform_scale_x(obj, 240, LV_STATE_PRESSED); // Shrink slightly to ~94% on press
    lv_obj_set_style_transform_scale_y(obj, 240, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, active_border_color, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(obj, glow_color, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(obj, 16, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_50, LV_STATE_PRESSED);
}

static void ui_setup_normal_click_effect(lv_obj_t * obj, lv_color_t active_border_color, lv_color_t glow_color)
{
    static const lv_style_prop_t transition_props[] = {
        LV_STYLE_BG_COLOR,
        LV_STYLE_BORDER_COLOR,
        LV_STYLE_SHADOW_WIDTH,
        LV_STYLE_SHADOW_COLOR,
        (lv_style_prop_t)0
    };
    
    static lv_style_transition_dsc_t transition_dsc;
    static bool transition_dsc_inited = false;
    if (!transition_dsc_inited) {
        lv_style_transition_dsc_init(&transition_dsc, transition_props, lv_anim_path_ease_out, 120, 0, NULL);
        transition_dsc_inited = true;
    }

    lv_obj_set_style_transition(obj, &transition_dsc, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);

    // Apply pressed state styles (glow/border but NO scale zoom)
    lv_obj_set_style_border_color(obj, active_border_color, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(obj, glow_color, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(obj, 16, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_50, LV_STATE_PRESSED);
}

static void set_modal_children_visible(lv_obj_t * modal, bool visible) {
    if (!modal) return;
    uint32_t cnt = lv_obj_get_child_count(modal);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t * child = lv_obj_get_child(modal, i);
        if (child) {
            if (visible) {
                lv_obj_remove_flag(child, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void open_modal_with_zoom_anim(lv_obj_t * modal) {
    if (!modal) return;
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_HIDDEN);
    set_modal_children_visible(modal, true);
    lv_obj_set_style_transform_scale_x(modal, 256, 0);
    lv_obj_set_style_transform_scale_y(modal, 256, 0);
}

static void close_modal_with_zoom_anim_delete(lv_obj_t ** modal_ptr) {
    if (!modal_ptr || !*modal_ptr) return;
    lv_obj_t * modal = *modal_ptr;
    *modal_ptr = NULL; // Clear pointer immediately to prevent double closes
    lv_obj_delete(modal);
}

static void close_modal_with_zoom_anim_hidden(lv_obj_t * modal) {
    if (!modal) return;
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);
    set_modal_children_visible(modal, true);
    lv_obj_set_style_transform_scale_x(modal, 256, 0);
    lv_obj_set_style_transform_scale_y(modal, 256, 0);
}

void ui_init(void) {
    /* Initialize SD card (required for wallpapers) */
    sd_card_bg_init();

    /* Configure CJK fallback font compiled directly in flash (ui_font_xiaozhi_14) */
    memcpy(&s_font_cjk_fallback, &lv_font_montserrat_14, sizeof(lv_font_t));
    s_font_cjk_fallback.fallback = &ui_font_xiaozhi_14;
    ESP_LOGI("UI", "Using Montserrat 14 with built-in CJK fallback font (ui_font_xiaozhi_14) in Flash.");

    /* Diagnostic: verify s_font_cjk_fallback finds a CJK glyph */
    {
        lv_font_glyph_dsc_t g = {0};
        bool ok = lv_font_get_glyph_dsc(&s_font_cjk_fallback, &g, 0x4E2D, 0);
        ESP_LOGI("UI", "Font diag: '中' lookup ok=%d placeholder=%d box=%dx%d",
                 ok, g.is_placeholder, g.box_w, g.box_h);
    }

    /* Initialize global no-shadow style ONCE */
    lv_style_init(&g_style_no_shadow);
    lv_style_set_shadow_width(&g_style_no_shadow, 0);
    lv_style_set_shadow_spread(&g_style_no_shadow, 0);
    lv_style_set_shadow_ofs_x(&g_style_no_shadow, 0);
    lv_style_set_shadow_ofs_y(&g_style_no_shadow, 0);

    /* Apply to root screens */
    lv_obj_add_style(lv_scr_act(), &g_style_no_shadow, 0);

    /* Initialize mutexes */
    if (s_device_app_mutex == NULL) {
        s_device_app_mutex = xSemaphoreCreateMutex();
    }
    if (s_udp_data_mutex == NULL) {
        s_udp_data_mutex = xSemaphoreCreateMutex();
    }

    /* SD card was initialized at the beginning of ui_init() */

    /* ====================================================================
     * 1. STARTUP SCREEN
     * ==================================================================== */
    scr_startup = lv_obj_create(NULL);
    apply_no_shadow(scr_startup);
    lv_obj_set_style_bg_color(scr_startup, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr_startup, LV_OPA_COVER, 0);

    /* Create persistent wallpaper image object at background */
    s_startup_wallpaper_img = lv_image_create(scr_startup);
    lv_obj_set_pos(s_startup_wallpaper_img, 0, 0);
    lv_obj_set_size(s_startup_wallpaper_img, 1024, 600);
    lv_obj_move_background(s_startup_wallpaper_img);

    /* Load startup wallpaper (slot 0) */
    if (sd_card_bg_is_ready()) {
        char saved_path[128] = {};
        const char *wp_path = "/sdcard/bg.jpg";
        if (load_startup_wallpaper_path(saved_path, sizeof(saved_path))) {
            wp_path = saved_path;
        }
        const lv_image_dsc_t *img = sd_card_bg_load_jpeg_slot(wp_path, 0);
        if (img) {
            lv_image_set_src(s_startup_wallpaper_img, img);
            strncpy(s_current_startup_wallpaper_path, wp_path, sizeof(s_current_startup_wallpaper_path) - 1);
            ESP_LOGI("UI", "Startup wallpaper loaded: %s", wp_path);
        } else {
            ESP_LOGW("UI", "Failed to load startup wallpaper: %s", wp_path);
        }
    }

    /* ====================================================================
     * 1.2 SYSTEM NEON ART TEXT "AI & ELEC"
     * ==================================================================== */
    s_neon_container = lv_obj_create(scr_startup);
    apply_no_shadow(s_neon_container);
    lv_obj_set_size(s_neon_container, 800, 180);
    lv_obj_align(s_neon_container, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_bg_opa(s_neon_container, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_neon_container, LV_OBJ_FLAG_SCROLLABLE);

    // 1. 最外层粉紫发光 (Neon Aura)
    s_neon_aura = lv_label_create(s_neon_container);
    lv_label_set_text(s_neon_aura, "AI FOR DESIGN");
    lv_obj_set_style_text_color(s_neon_aura, lv_color_hex(0xFF00FF), 0);
    lv_obj_set_style_text_font(s_neon_aura, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_opa(s_neon_aura, LV_OPA_40, 0);
    lv_obj_center(s_neon_aura);

    // 2. 中层冰蓝发光 (Neon Glow)
    s_neon_glow = lv_label_create(s_neon_container);
    lv_label_set_text(s_neon_glow, "AI FOR DESIGN");
    lv_obj_set_style_text_color(s_neon_glow, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_text_font(s_neon_glow, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_opa(s_neon_glow, LV_OPA_60, 0);
    lv_obj_center(s_neon_glow);

    // 3. 顶层白色实体字 (Neon Core)
    s_neon_core = lv_label_create(s_neon_container);
    lv_label_set_text(s_neon_core, "AI FOR DESIGN");
    lv_obj_set_style_text_color(s_neon_core, lv_color_hex(0xE0FFFF), 0);
    lv_obj_set_style_text_font(s_neon_core, &lv_font_montserrat_48, 0);
    lv_obj_center(s_neon_core);

    lv_anim_t a_neon_glow;
    lv_anim_init(&a_neon_glow);
    lv_anim_set_var(&a_neon_glow, s_neon_glow);
    lv_anim_set_values(&a_neon_glow, LV_OPA_30, LV_OPA_100);
    lv_anim_set_duration(&a_neon_glow, 1200);
    lv_anim_set_playback_duration(&a_neon_glow, 1200);
    lv_anim_set_repeat_count(&a_neon_glow, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a_neon_glow, neon_anim_glow_cb);
    lv_anim_start(&a_neon_glow);

    lv_anim_t a_neon_aura;
    lv_anim_init(&a_neon_aura);
    lv_anim_set_var(&a_neon_aura, s_neon_aura);
    lv_anim_set_values(&a_neon_aura, LV_OPA_10, LV_OPA_90);
    lv_anim_set_duration(&a_neon_aura, 600);
    lv_anim_set_playback_duration(&a_neon_aura, 600);
    lv_anim_set_repeat_count(&a_neon_aura, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a_neon_aura, neon_anim_aura_cb);
    lv_anim_start(&a_neon_aura);

    lv_anim_t a_neon_glow_x;
    lv_anim_init(&a_neon_glow_x);
    lv_anim_set_var(&a_neon_glow_x, s_neon_glow);
    lv_anim_set_values(&a_neon_glow_x, -10, 10);
    lv_anim_set_duration(&a_neon_glow_x, 1500);
    lv_anim_set_playback_duration(&a_neon_glow_x, 1500);
    lv_anim_set_repeat_count(&a_neon_glow_x, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a_neon_glow_x, neon_anim_glow_x_cb);
    lv_anim_start(&a_neon_glow_x);

    lv_anim_t a_neon_aura_y;
    lv_anim_init(&a_neon_aura_y);
    lv_anim_set_var(&a_neon_aura_y, s_neon_aura);
    lv_anim_set_values(&a_neon_aura_y, -6, 6);
    lv_anim_set_duration(&a_neon_aura_y, 300);
    lv_anim_set_playback_duration(&a_neon_aura_y, 300);
    lv_anim_set_repeat_count(&a_neon_aura_y, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a_neon_aura_y, neon_anim_aura_y_cb);
    lv_anim_start(&a_neon_aura_y);

    btn_wifi = lv_btn_create(scr_startup);
    lv_obj_set_size(btn_wifi, 260, 48);
    lv_obj_align(btn_wifi, LV_ALIGN_BOTTOM_LEFT, 80, -40);
    lv_obj_set_style_bg_color(btn_wifi, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(btn_wifi, COLOR_BORDER, 0);
    // lv_obj_set_style_border_width(btn_wifi, 1, 0);  // DISABLED for crash diagnostic
    lv_obj_set_style_radius(btn_wifi, 24, 0);
    lv_obj_add_event_cb(btn_wifi, btn_open_wifi_cb, LV_EVENT_CLICKED, NULL);
    wifi_status_label = lv_label_create(btn_wifi);
    lv_label_set_text(wifi_status_label, "WiFi: Disconnected");
    lv_obj_set_style_text_font(wifi_status_label, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(wifi_status_label, COLOR_TEXT, 0);
    lv_obj_center(wifi_status_label);

    lv_obj_t * btn_wallpaper = lv_btn_create(scr_startup);
    lv_obj_set_size(btn_wallpaper, 260, 48);
    lv_obj_align(btn_wallpaper, LV_ALIGN_BOTTOM_RIGHT, -80, -40);
    lv_obj_set_style_bg_color(btn_wallpaper, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(btn_wallpaper, lv_color_hex(0xC084FC), 0);
    // lv_obj_set_style_border_width(btn_wallpaper, 1, 0);  // DISABLED for crash diagnostic
    lv_obj_set_style_radius(btn_wallpaper, 24, 0);
    lv_obj_add_event_cb(btn_wallpaper, [](lv_event_t * e) {
        s_wallpaper_target = WALLPAPER_TARGET_STARTUP;
        open_wallpaper_list();
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_wp_lbl = lv_label_create(btn_wallpaper);
    lv_label_set_text(btn_wp_lbl, LV_SYMBOL_IMAGE " Wallpaper");
    lv_obj_set_style_text_color(btn_wp_lbl, lv_color_hex(0xC084FC), 0);
    lv_obj_center(btn_wp_lbl);

    lv_obj_t * btn_enter = lv_btn_create(scr_startup);
    lv_obj_set_size(btn_enter, 260, 48);
    lv_obj_align(btn_enter, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(btn_enter, COLOR_ACCENT, 0);
    lv_obj_set_style_radius(btn_enter, 24, 0);
    lv_obj_add_event_cb(btn_enter, btn_enter_event_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_label = lv_label_create(btn_enter);
    lv_label_set_text(btn_label, "Enter Dashboard");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(btn_label);

    create_wifi_modals();
    create_wallpaper_modal();

    /* ====================================================================
     * 1.5 SYSTEM CHECK STATUS CAPSULE (灵动岛) & BREATHING LED
     * ==================================================================== */
    build_island(scr_startup, &s_island);

    /* Island border flow timer */
    s_island_flow_timer = lv_timer_create(island_flow_timer_cb, 120, &s_island);

    s_startup_check_index = 0;
    s_startup_check_timer = lv_timer_create(startup_check_timer_cb, 2500, NULL);

    /* ====================================================================
     * 2. MAIN DASHBOARD - SD CARD BACKGROUND + MINIMAL UI
     * ==================================================================== */
    scr_main = lv_obj_create(NULL);
    apply_no_shadow(scr_main);
    lv_obj_set_style_bg_color(scr_main, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr_main, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_main, main_screen_event_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr_main, main_screen_event_cb, LV_EVENT_PRESSED, NULL);

    /* Create persistent wallpaper image object at background */
    s_wallpaper_img = lv_image_create(scr_main);
    lv_obj_set_pos(s_wallpaper_img, 0, 0);
    lv_obj_set_size(s_wallpaper_img, 1024, 600);
    lv_obj_move_background(s_wallpaper_img);

    /* Load SD card background image (slot 1) */
    if (sd_card_bg_is_ready()) {
        char saved_path[128] = "";
        const char *wallpaper_path = "/sdcard/bg.jpg";
        if (load_wallpaper_path(saved_path, sizeof(saved_path))) {
            wallpaper_path = saved_path;
        }
        const lv_image_dsc_t *img = sd_card_bg_load_jpeg_slot(wallpaper_path, 1);
        if (img) {
            lv_image_set_src(s_wallpaper_img, img);
            strncpy(s_current_wallpaper_path, wallpaper_path, sizeof(s_current_wallpaper_path) - 1);
            ESP_LOGI("UI", "Main screen background loaded: %s", wallpaper_path);
        } else {
            ESP_LOGW("UI", "Failed to load wallpaper from SD card: %s", wallpaper_path);
        }
    } else {
        ESP_LOGW("UI", "SD card init failed, using dark background");
    }

    /* Dynamic island (same as startup, lives on main screen) */
    build_island(scr_main, &m_island);

    /* Right side: Apps drawer (hidden, swipe left/right to open/close) */
    apps_panel = lv_obj_create(scr_main);
    apply_no_shadow(apps_panel);
    lv_obj_set_size(apps_panel, 200, 440);
    lv_obj_set_pos(apps_panel, 1024, 20);
    lv_obj_set_style_bg_color(apps_panel, lv_color_hex(0x0C1226), 0);
    lv_obj_set_style_bg_opa(apps_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_color(apps_panel, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_style_border_opa(apps_panel, LV_OPA_30, 0);
    lv_obj_set_style_border_width(apps_panel, 1, 0);
    lv_obj_set_style_radius(apps_panel, 24, 0);
    lv_obj_set_style_pad_all(apps_panel, 0, 0);
    lv_obj_clear_flag(apps_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Side drawer header: accent bar + title */
    lv_obj_t * title_bar = lv_obj_create(apps_panel);
    apply_no_shadow(title_bar);
    lv_obj_set_size(title_bar, 4, 16);
    lv_obj_set_pos(title_bar, 24, 16);
    lv_obj_set_style_bg_color(title_bar, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(title_bar, 2, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * side_title = lv_label_create(apps_panel);
    lv_label_set_text(side_title, "快捷侧边栏");
    lv_obj_set_style_text_font(side_title, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(side_title, lv_color_hex(0x00F0FF), 0);
    lv_obj_align(side_title, LV_ALIGN_TOP_LEFT, 36, 12);

    /* Separator line 1 */
    lv_obj_t * sep1 = lv_obj_create(apps_panel);
    apply_no_shadow(sep1);
    lv_obj_set_size(sep1, 168, 1);
    lv_obj_set_pos(sep1, 16, 40);
    lv_obj_set_style_bg_color(sep1, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_bg_opa(sep1, LV_OPA_70, 0);
    lv_obj_set_style_border_width(sep1, 0, 0);

    /* Drawer grip handle (always visible hint on right edge) */
    lv_obj_t * grip = lv_obj_create(scr_main);
    apply_no_shadow(grip);
    lv_obj_set_size(grip, 8, 100);
    lv_obj_set_pos(grip, 1016, 190);
    lv_obj_set_style_bg_color(grip, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_80, 0);
    lv_obj_set_style_radius(grip, 4, 0);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(grip, [](lv_event_t * e){ toggle_apps_drawer(true); }, LV_EVENT_CLICKED, NULL);
    for (int i = 0; i < 3; i++) {
        lv_obj_t * dot = lv_obj_create(grip);
        apply_no_shadow(dot);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x00F0FF), 0);
        lv_obj_set_style_bg_opa(dot, (i == 1) ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_style_radius(dot, 2, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 18 + i * 30);
    }

    /* S3-A / S3-B will be created dynamically on first sensor data arrival */

    /* Camera app icon */
    btn_qr_entry = lv_btn_create(apps_panel);
    lv_obj_set_size(btn_qr_entry, 80, 80);
    lv_obj_set_pos(btn_qr_entry, 16, 142);
    lv_obj_set_style_bg_color(btn_qr_entry, lv_color_hex(0x16223E), 0);
    lv_obj_set_style_bg_opa(btn_qr_entry, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_qr_entry, lv_color_hex(0x4AA3FF), 0);
    lv_obj_set_style_border_opa(btn_qr_entry, LV_OPA_60, 0);
    lv_obj_set_style_border_width(btn_qr_entry, 1, 0);
    lv_obj_set_style_radius(btn_qr_entry, 20, 0);
    lv_obj_set_style_shadow_color(btn_qr_entry, lv_color_hex(0x4AA3FF), 0);
    lv_obj_set_style_shadow_width(btn_qr_entry, 14, 0);
    lv_obj_set_style_shadow_opa(btn_qr_entry, LV_OPA_30, 0);
    
    lv_obj_t * cam_sym = lv_label_create(btn_qr_entry);
    lv_label_set_text(cam_sym, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(cam_sym, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cam_sym, lv_color_hex(0x4AA3FF), 0);
    lv_obj_align(cam_sym, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_t * cam_label = lv_label_create(btn_qr_entry);
    lv_label_set_text(cam_label, "Camera");
    lv_obj_set_style_text_font(cam_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cam_label, COLOR_TEXT, 0);
    lv_obj_align(cam_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* Separator line 2 */
    lv_obj_t * sep2 = lv_obj_create(apps_panel);
    apply_no_shadow(sep2);
    lv_obj_set_size(sep2, 168, 1);
    lv_obj_set_pos(sep2, 16, 238);
    lv_obj_set_style_bg_color(sep2, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_70, 0);
    lv_obj_set_style_border_width(sep2, 0, 0);

    /* System Stats Title with accent dot */
    lv_obj_t * stat_dot = lv_obj_create(apps_panel);
    apply_no_shadow(stat_dot);
    lv_obj_set_size(stat_dot, 4, 4);
    lv_obj_set_pos(stat_dot, 24, 252);
    lv_obj_set_style_bg_color(stat_dot, lv_color_hex(0x91A0B8), 0);
    lv_obj_set_style_bg_opa(stat_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stat_dot, 2, 0);
    lv_obj_set_style_border_width(stat_dot, 0, 0);
    lv_obj_clear_flag(stat_dot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * widget_title = lv_label_create(apps_panel);
    lv_label_set_text(widget_title, "系统监测");
    lv_obj_set_style_text_font(widget_title, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(widget_title, lv_color_hex(0x91A0B8), 0);
    lv_obj_align(widget_title, LV_ALIGN_TOP_LEFT, 34, 246);

    /* Circular Memory Arc Widget */
    side_mem_arc = lv_arc_create(apps_panel);
    lv_obj_set_size(side_mem_arc, 74, 74);
    lv_obj_set_pos(side_mem_arc, 18, 273);
    lv_arc_set_bg_angles(side_mem_arc, 135, 45);
    lv_arc_set_value(side_mem_arc, 0);
    lv_obj_remove_style(side_mem_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(side_mem_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(side_mem_arc, lv_color_hex(0x192543), LV_PART_MAIN);
    lv_obj_set_style_arc_color(side_mem_arc, lv_color_hex(0x00F0FF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(side_mem_arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(side_mem_arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(side_mem_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(side_mem_arc, true, LV_PART_INDICATOR);

    side_mem_lbl = lv_label_create(apps_panel);
    lv_label_set_text(side_mem_lbl, "0%");
    lv_obj_set_style_text_font(side_mem_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(side_mem_lbl, lv_color_hex(0x00F0FF), 0);
    lv_obj_align_to(side_mem_lbl, side_mem_arc, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * mem_desc = lv_label_create(apps_panel);
    lv_label_set_text(mem_desc, "内存使用");
    lv_obj_set_style_text_font(mem_desc, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(mem_desc, lv_color_hex(0x91A0B8), 0);
    lv_obj_align_to(mem_desc, side_mem_arc, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    /* Environment Sensor Card Widget */
    lv_obj_t * env_card = lv_obj_create(apps_panel);
    apply_no_shadow(env_card);
    lv_obj_set_size(env_card, 82, 74);
    lv_obj_set_pos(env_card, 102, 273);
    lv_obj_set_style_bg_color(env_card, lv_color_hex(0x16223E), 0);
    lv_obj_set_style_bg_opa(env_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(env_card, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(env_card, 1, 0);
    lv_obj_set_style_radius(env_card, 12, 0);
    lv_obj_set_style_pad_all(env_card, 4, 0);
    lv_obj_clear_flag(env_card, LV_OBJ_FLAG_SCROLLABLE);

    side_temp_lbl = lv_label_create(env_card);
    lv_label_set_text(side_temp_lbl, "温度: --°C");
    lv_obj_set_style_text_font(side_temp_lbl, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(side_temp_lbl, lv_color_hex(0xFFB84D), 0);
    lv_obj_align(side_temp_lbl, LV_ALIGN_TOP_MID, 0, 6);

    side_humi_lbl = lv_label_create(env_card);
    lv_label_set_text(side_humi_lbl, "湿度: --%");
    lv_obj_set_style_text_font(side_humi_lbl, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(side_humi_lbl, lv_color_hex(0x03DAC6), 0);
    lv_obj_align(side_humi_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* Separator line 3 */
    lv_obj_t * sep3 = lv_obj_create(apps_panel);
    apply_no_shadow(sep3);
    lv_obj_set_size(sep3, 168, 1);
    lv_obj_set_pos(sep3, 16, 360);
    lv_obj_set_style_bg_color(sep3, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_bg_opa(sep3, LV_OPA_50, 0);
    lv_obj_set_style_border_width(sep3, 0, 0);

    /* Footer hint */
    lv_obj_t * side_footer = lv_label_create(apps_panel);
    lv_label_set_text(side_footer, "向右滑动收起");
    lv_obj_set_style_text_font(side_footer, &s_font_cjk_fallback, 0);
    lv_obj_set_style_text_color(side_footer, lv_color_hex(0x4A5A7A), 0);
    lv_obj_align(side_footer, LV_ALIGN_BOTTOM_MID, 0, -14);

#if 0 /* CONFIG_ELEC_NOSE_GATEWAY_ENABLE - Removed permanently per user request */
    /* AI Gateway app icon */
    ai_analyze_btn = lv_btn_create(apps_panel);
    lv_obj_set_size(ai_analyze_btn, 80, 80);
    lv_obj_set_pos(ai_analyze_btn, 104, 112);
    lv_obj_set_style_bg_color(ai_analyze_btn, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_border_color(ai_analyze_btn, lv_color_hex(0x39D98A), 0);
    lv_obj_set_style_border_width(ai_analyze_btn, 2, 0);
    lv_obj_set_style_radius(ai_analyze_btn, 18, 0);
    lv_obj_add_event_cb(ai_analyze_btn, ai_analyze_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ai_sym = lv_label_create(ai_analyze_btn);
    lv_label_set_text(ai_sym, LV_SYMBOL_EDIT);
    lv_obj_set_style_text_font(ai_sym, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ai_sym, lv_color_hex(0x39D98A), 0);
    lv_obj_align(ai_sym, LV_ALIGN_TOP_MID, 0, 12);
    ai_analyze_btn_label = lv_label_create(ai_analyze_btn);
    lv_label_set_text(ai_analyze_btn_label, "AI");
    lv_obj_set_style_text_color(ai_analyze_btn_label, COLOR_TEXT, 0);
    lv_obj_align(ai_analyze_btn_label, LV_ALIGN_BOTTOM_MID, 0, -8);
#endif

    /* Model status indicator (top-left corner, inline with status bar) - Enlarge for easy touch */
    main_model_card = lv_obj_create(scr_main);
    apply_no_shadow(main_model_card);
    lv_obj_set_size(main_model_card, 300, 42); // 宽 200->300, 高 28->42
    lv_obj_set_pos(main_model_card, 15, 15);   // 位置微调 (10, 14)->(15, 15)
    lv_obj_set_style_bg_color(main_model_card, lv_color_hex(0x131828), 0);
    lv_obj_set_style_bg_opa(main_model_card, LV_OPA_60, 0);
    lv_obj_set_style_border_color(main_model_card, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(main_model_card, 1, 0);
    lv_obj_set_style_radius(main_model_card, 21, 0); // 半圆圆角 14->21
    lv_obj_clear_flag(main_model_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(main_model_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(main_model_card, open_model_switch_modal_cb, LV_EVENT_CLICKED, NULL);
    ui_setup_normal_click_effect(main_model_card, lv_color_hex(0xFFB84D), lv_color_hex(0xFFB84D));

    main_model_status_led = lv_led_create(main_model_card);
    lv_obj_set_size(main_model_status_led, 12, 12); // 灯泡大小 8x8->12x12
    lv_obj_align(main_model_status_led, LV_ALIGN_LEFT_MID, 12, 0); // 留白从 8->12
    if (g_receiver_model_ready) {
        lv_led_on(main_model_status_led);
    } else {
        lv_led_off(main_model_status_led);
    }

    main_model_name_label = lv_label_create(main_model_card);
    lv_label_set_text_fmt(main_model_name_label, "%s v%s", g_receiver_model_name, g_receiver_model_version);
    lv_obj_set_style_text_color(main_model_name_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(main_model_name_label, &lv_font_montserrat_14, 0); // 字体大小 montserrat_12->montserrat_14
    lv_obj_align(main_model_name_label, LV_ALIGN_LEFT_MID, 32, 0); // 避开灯泡 22->32

    main_model_bar = lv_bar_create(main_model_card);
    lv_obj_set_size(main_model_bar, 280, 4);
    lv_obj_align(main_model_bar, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_set_style_bg_color(main_model_bar, lv_color_hex(0x192543), LV_PART_MAIN);
    lv_obj_set_style_radius(main_model_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(main_model_bar, 2, LV_PART_INDICATOR);
    lv_obj_add_flag(main_model_bar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * switch_arrow = lv_label_create(main_model_card);
    lv_label_set_text(switch_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(switch_arrow, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(switch_arrow, &lv_font_montserrat_14, 0); // 箭头字体变大
    lv_obj_align(switch_arrow, LV_ALIGN_RIGHT_MID, -12, 0); // 留白 -6->-12



    /* Upload button (just right of model indicator) - Enlarge to match capsule height */
    lv_obj_t * upload_btn = lv_btn_create(scr_main);
    lv_obj_set_size(upload_btn, 42, 42); // 宽 28->42, 高 28->42
    lv_obj_set_pos(upload_btn, 325, 15); // 右移至胶囊右侧，(15+300+10 = 325)
    lv_obj_set_style_bg_color(upload_btn, lv_color_hex(0x131828), 0);
    lv_obj_set_style_bg_opa(upload_btn, LV_OPA_60, 0);
    lv_obj_set_style_border_color(upload_btn, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(upload_btn, 1, 0);
    lv_obj_set_style_radius(upload_btn, 21, 0); // 半圆 14->21
    lv_obj_clear_flag(upload_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * upload_icon = lv_label_create(upload_btn);
    lv_label_set_text(upload_icon, LV_SYMBOL_UPLOAD);
    lv_obj_set_style_text_color(upload_icon, lv_color_hex(0xFFB84D), 0);
    lv_obj_set_style_text_font(upload_icon, &lv_font_montserrat_16, 0); // 图标稍微变大
    lv_obj_center(upload_icon);
    lv_obj_add_event_cb(upload_btn, [](lv_event_t * e) {
        LV_UNUSED(e);
        cloud_sync_trigger_once();
    }, LV_EVENT_CLICKED, NULL);
    ui_setup_premium_click_effect(upload_btn, lv_color_hex(0xFFB84D), lv_color_hex(0xFFB84D));

    /* Training button (just right of upload button) - Trigger model training */
    lv_obj_t * train_btn = lv_btn_create(scr_main);
    lv_obj_set_size(train_btn, 42, 42);
    lv_obj_set_pos(train_btn, 377, 15); // x = 325 + 42 + 10 = 377
    lv_obj_set_style_bg_color(train_btn, lv_color_hex(0x131828), 0);
    lv_obj_set_style_bg_opa(train_btn, LV_OPA_60, 0);
    lv_obj_set_style_border_color(train_btn, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(train_btn, 1, 0);
    lv_obj_set_style_radius(train_btn, 21, 0);
    lv_obj_clear_flag(train_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * train_icon = lv_label_create(train_btn);
    lv_label_set_text(train_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(train_icon, lv_color_hex(0x63B3ED), 0); // Tech light-blue
    lv_obj_set_style_text_font(train_icon, &lv_font_montserrat_16, 0);
    lv_obj_center(train_icon);
    lv_obj_add_event_cb(train_btn, open_training_modal_cb, LV_EVENT_CLICKED, NULL);
    ui_setup_normal_click_effect(train_btn, lv_color_hex(0x63B3ED), lv_color_hex(0x63B3ED));


    /* Bottom bar: Volume only */
    lv_obj_t * bottom_bar = lv_obj_create(scr_main);
    apply_no_shadow(bottom_bar);
    lv_obj_set_size(bottom_bar, 1024, 56);
    lv_obj_align(bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bottom_bar, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bottom_bar, 0, 0);
    lv_obj_set_style_radius(bottom_bar, 0, 0);
    lv_obj_clear_flag(bottom_bar, LV_OBJ_FLAG_SCROLLABLE);



    /* Create Siri orb on scr_main (not lazy — event-handler creation caused freeze) */
    create_siri_orb();

    /* Fake data timer for UI status updates (created here, not in btn handler, to avoid freeze) */
    fake_data_timer = lv_timer_create(fake_data_timer_cb, 500, NULL);

    control_center_init(scr_main);

    /* ====================================================================
     * 3. QR SCAN SCREEN
     * ==================================================================== */
    scr_qr = lv_obj_create(NULL);
    apply_no_shadow(scr_qr);
    lv_obj_set_style_bg_color(scr_qr, lv_color_hex(0x0A0A1A), 0);
    lv_obj_set_style_bg_opa(scr_qr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr_qr, sub_screen_back_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr_qr, sub_screen_back_gesture_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr_qr, [](lv_event_t * e){
        /* Stop camera callback immediately (non-blocking) */
        camera_display_initialized = false;
        camera_stream_stopping = true;
        /* Defer heavy cleanup to a separate task to avoid blocking LVGL screen switch */
        if (!s_camera_cleanup_pending) {
            s_camera_cleanup_pending = true;
            xTaskCreate(camera_cleanup_task, "cam_cleanup", 3072, NULL, 2, NULL);
        }
    }, LV_EVENT_SCREEN_UNLOAD_START, NULL);

    cam_img_obj = lv_img_create(scr_qr);
    lv_obj_set_pos(cam_img_obj, 0, 60);
    lv_obj_set_size(cam_img_obj, 1024, 540);

    lv_obj_t * qr_title = lv_label_create(scr_qr);
    lv_label_set_text(qr_title, "E-Nose Node Scanner");
    lv_obj_set_style_text_color(qr_title, lv_color_hex(0x00FF88), 0);
    lv_obj_align(qr_title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t * btn_qr_back = lv_btn_create(scr_qr);
    lv_obj_set_pos(btn_qr_back, 10, 60);
    lv_obj_set_size(btn_qr_back, 80, 40);
    lv_obj_set_style_bg_color(btn_qr_back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_qr_back, 5, 0);
    lv_obj_t * lbl_qr_back = lv_label_create(btn_qr_back);
    lv_label_set_text(lbl_qr_back, "< Back");
    lv_obj_set_style_text_color(lbl_qr_back, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_qr_back);

    lv_obj_t * scan_hint = lv_label_create(scr_qr);
    lv_label_set_text(scan_hint, "Place QR in camera view");
    lv_obj_set_style_text_color(scan_hint, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(scan_hint, LV_ALIGN_BOTTOM_MID, 0, -100);

    lv_obj_t * scan_box = lv_obj_create(scr_qr);
    apply_no_shadow(scan_box);
    lv_obj_set_pos(scan_box, 412, 200);
    lv_obj_set_size(scan_box, 200, 200);
    lv_obj_set_style_bg_opa(scan_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(scan_box, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(scan_box, 3, 0);
    lv_obj_set_style_radius(scan_box, 10, 0);

    qr_result_panel = lv_obj_create(scr_qr);
    apply_no_shadow(qr_result_panel);
    lv_obj_set_size(qr_result_panel, 200, 100);
    lv_obj_align(qr_result_panel, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(qr_result_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(qr_result_panel, LV_OPA_70, 0);
    lv_obj_set_style_border_color(qr_result_panel, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_border_width(qr_result_panel, 1, 0);
    lv_obj_set_style_radius(qr_result_panel, 10, 0);
    lv_obj_add_flag(qr_result_panel, LV_OBJ_FLAG_HIDDEN);
    
    qr_result_label = lv_label_create(qr_result_panel);
    lv_label_set_text(qr_result_label, "Waiting for QR...");
    lv_obj_set_style_text_color(qr_result_label, lv_color_hex(0x00FF88), 0);
    lv_obj_align(qr_result_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_add_event_cb(btn_qr_entry, [](lv_event_t * e){
        ESP_LOGI("CAM", "QR Scanner button clicked");
        /* If a previous cleanup is still running, wait for it to finish */
        int wait = 0;
        while (s_camera_cleanup_pending && wait < 40) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait++;
        }
        lv_screen_load(scr_qr);
        lv_obj_add_flag(qr_result_panel, LV_OBJ_FLAG_HIDDEN);
        camera_display_initialized = false;
        camera_stream_stopping = false;
        cam_scale_applied = false;
        if(video_fd < 0) {
            ESP_LOGI("CAM", "Opening camera device...");
            if(!camera_initialized) {
                i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
                esp_err_t ret1 = app_video_main(i2c_bus);
                if (ret1 != ESP_OK) {
                    ESP_LOGE("CAM", "Camera sensor init failed: %s", esp_err_to_name(ret1));
                } else {
                    ESP_LOGI("CAM", "Camera sensor initialized successfully");
                    camera_initialized = true;
                }
            } else {
                ESP_LOGI("CAM", "Camera already initialized");
            }
            
            app_video_register_frame_operation_cb(camera_frame_cb);
            ESP_LOGI("CAM", "Frame callback registered");

            /* Let camera use its default resolution (sensor's native config) */
            g_video_req_width = 0;
            g_video_req_height = 0;
            
            video_fd = app_video_open((char*)"/dev/video0", (video_fmt_t)APP_VIDEO_FMT_RGB565);
            if(video_fd < 0) {
                ESP_LOGE("CAM", "Failed to open camera device");
                return;
            }
            ESP_LOGI("CAM", "Camera device opened, fd=%d", video_fd);
            
            esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &cam_alignment);
            cam_buf_size = app_video_get_buf_size();
            ESP_LOGI("CAM", "Buffer size: %zu bytes, alignment: %zu", cam_buf_size, cam_alignment);
            
            if(cam_buf_size == 0) {
                ESP_LOGE("CAM", "Invalid buffer size");
                close(video_fd);
                video_fd = -1;
                return;
            }
            
            for(int i = 0; i < CAM_BUF_NUM; i++) {
                cam_buffers[i] = (uint8_t *)heap_caps_aligned_alloc(cam_alignment, cam_buf_size, MALLOC_CAP_SPIRAM);
                if(!cam_buffers[i]) {
                    ESP_LOGE("CAM", "Failed to allocate buffer %d", i);
                    for(int j = 0; j < i; j++) {
                        heap_caps_free(cam_buffers[j]);
                        cam_buffers[j] = NULL;
                    }
                    close(video_fd);
                    video_fd = -1;
                    return;
                }
                ESP_LOGI("CAM", "Buffer %d allocated at %p", i, cam_buffers[i]);
            }
            
            ESP_LOGI("CAM", "Setting up %d buffers in USERPTR mode...", CAM_BUF_NUM);
            esp_err_t ret2 = app_video_set_bufs(video_fd, CAM_BUF_NUM, (const void **)cam_buffers);
            if(ret2 != ESP_OK) {
                ESP_LOGE("CAM", "Failed to set video buffers: %s", esp_err_to_name(ret2));
                for(int i = 0; i < CAM_BUF_NUM; i++) {
                    if(cam_buffers[i]) heap_caps_free(cam_buffers[i]);
                }
                close(video_fd);
                video_fd = -1;
                return;
            }
            
            ESP_LOGI("CAM", "Starting video stream task...");
            esp_err_t ret3 = app_video_stream_task_start(video_fd, 0);
            if(ret3 != ESP_OK) {
                ESP_LOGE("CAM", "Failed to start video stream task: %s", esp_err_to_name(ret3));
                for(int i = 0; i < CAM_BUF_NUM; i++) {
                    if(cam_buffers[i]) heap_caps_free(cam_buffers[i]);
                    cam_buffers[i] = NULL;
                }
                close(video_fd);
                video_fd = -1;
                // Keep camera_initialized = true: esp_video_init() already registered ISP device,
                // calling it again would fail with "video name=ISP has been registered"
                return;
            }
            
            ESP_LOGI("CAM", "Camera stream started successfully");

            /* Create QR processing semaphores and task */
            if (!qr_write_sem) {
                qr_write_sem = xSemaphoreCreateBinary();
                xSemaphoreGive(qr_write_sem);  /* initially available for writing */
            } else {
                xSemaphoreTake(qr_write_sem, 0);
                xSemaphoreGive(qr_write_sem);
            }
            if (!qr_read_sem) {
                qr_read_sem = xSemaphoreCreateBinary();
            } else {
                xSemaphoreTake(qr_read_sem, 0);
            }
            if (!qr_task_running && qr_write_sem && qr_read_sem) {
                qr_task_running = true;
                if (!qr_task_stack) {
                    qr_task_stack = (StackType_t *)heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
                }
                if (qr_task_stack) {
                    qr_task_handle = xTaskCreateStaticPinnedToCore(
                        qr_scan_task, "qr_scan", 32768 / sizeof(StackType_t),
                        NULL, 2, qr_task_stack, &qr_task_buf, 1);
                    ESP_LOGI("QR", "QR scan task created on core 1 (stack=%p)", qr_task_stack);
                } else {
                    ESP_LOGW("QR", "Failed to allocate QR task stack");
                }
            }

            vTaskDelay(pdMS_TO_TICKS(500));
            camera_display_initialized = true;
        } else {
            ESP_LOGI("CAM", "Camera already running (fd=%d)", video_fd);
            camera_display_initialized = true;
        }
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(btn_qr_back, [](lv_event_t * e){
        lv_screen_load(scr_main);
    }, LV_EVENT_CLICKED, NULL);

    /* ====================================================================
     * 4. SHARED DEVICE DETAIL SCREEN
     * ==================================================================== */

        scr_device_detail = lv_obj_create(NULL);
    apply_no_shadow(scr_device_detail);
    lv_obj_set_style_bg_color(scr_device_detail, lv_color_hex(0x0A0F1D), 0); // Dark space background
    lv_obj_set_style_bg_opa(scr_device_detail, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr_device_detail, sub_screen_back_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr_device_detail, sub_screen_back_gesture_cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t * detail_back = lv_btn_create(scr_device_detail);
    lv_obj_set_pos(detail_back, 15, 15);
    lv_obj_set_size(detail_back, 90, 40);
    lv_obj_set_style_bg_color(detail_back, lv_color_hex(0x1E2942), 0);
    lv_obj_set_style_border_color(detail_back, lv_color_hex(0x3B4F7A), 0);
    lv_obj_set_style_border_width(detail_back, 1, 0);
    lv_obj_set_style_radius(detail_back, 10, 0);
    lv_obj_t * detail_back_lbl = lv_label_create(detail_back);
    lv_label_set_text(detail_back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(detail_back_lbl, lv_color_hex(0xE6ECF5), 0);
    lv_obj_center(detail_back_lbl);
    lv_obj_add_event_cb(detail_back, [](lv_event_t * e){ lv_screen_load(scr_main); }, LV_EVENT_CLICKED, NULL);

    device_detail_title = lv_label_create(scr_device_detail);
    lv_label_set_text(device_detail_title, "Device Sensor Dashboard");
    lv_obj_set_style_text_font(device_detail_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(device_detail_title, lv_color_hex(0x4AA3FF), 0);
    lv_obj_align(device_detail_title, LV_ALIGN_TOP_MID, 0, 16);

    // Parent Glassmorphic Card Container
    lv_obj_t * detail_card = lv_obj_create(scr_device_detail);
    apply_no_shadow(detail_card);
    lv_obj_set_size(detail_card, 900, 480);
    lv_obj_align(detail_card, LV_ALIGN_CENTER, 0, 25);
    lv_obj_set_style_bg_color(detail_card, lv_color_hex(0x111625), 0);
    lv_obj_set_style_bg_opa(detail_card, LV_OPA_80, 0);
    lv_obj_set_style_border_color(detail_card, lv_color_hex(0x2A3E60), 0);
    lv_obj_set_style_border_width(detail_card, 1, 0);
    lv_obj_set_style_radius(detail_card, 24, 0);

    /* --- LEFT SIDE: 3x2 Sensor Grid (Width 560px) --- */
    struct {
        const char *title;
        lv_color_t color;
        int x, y;
        lv_obj_t **lbl;
    } cards[] = {
        {"VOC Level", lv_color_hex(0x4AA3FF), 25, 20, &d_voc_label},
        {"CO2 Reading", lv_color_hex(0x39D98A), 205, 20, &d_co2_label},
        {"Odor Intensity", lv_color_hex(0xFFB84D), 385, 20, &d_odor_label},
        {"HCHO Concentration", lv_color_hex(0xFF5533), 25, 135, &d_hcho_label},
        {"CO Density", lv_color_hex(0x8C52FF), 205, 135, &d_co_label},
        {"AI Prediction", lv_color_hex(0x2EE59D), 385, 135, &d_pred_label}
    };

    for (int i = 0; i < 6; i++) {
        lv_obj_t *c = lv_obj_create(detail_card);
        apply_no_shadow(c);
        lv_obj_set_size(c, 160, 110);
        lv_obj_set_pos(c, cards[i].x, cards[i].y);
        lv_obj_set_style_bg_color(c, lv_color_hex(0x181F33), 0);
        lv_obj_set_style_border_color(c, lv_color_hex(0x283552), 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_radius(c, 16, 0);

        // Colored status indicator dot
        lv_obj_t *dot = lv_obj_create(c);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, cards[i].color, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_pos(dot, 10, 10);

        // Small muted card description label
        lv_obj_t *lbl_title = lv_label_create(c);
        lv_label_set_text(lbl_title, cards[i].title);
        lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x8FA3B8), 0);
        lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(lbl_title, 24, 8);

        // Dynamic value label (centered in card)
        *(cards[i].lbl) = lv_label_create(c);
        lv_label_set_text(*(cards[i].lbl), "--");
        lv_obj_set_style_text_color(*(cards[i].lbl), lv_color_hex(0xE6ECF5), 0);
        lv_obj_set_style_text_font(*(cards[i].lbl), &lv_font_montserrat_14, 0);
        lv_obj_align(*(cards[i].lbl), LV_ALIGN_BOTTOM_LEFT, 10, -15);
    }

    // 6. VOC Trend Chart (Left Bottom - Space saving)
    s_detail_chart = lv_chart_create(detail_card);
    lv_obj_set_pos(s_detail_chart, 25, 255);
    lv_obj_set_size(s_detail_chart, 520, 100);
    lv_chart_set_type(s_detail_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_detail_chart, 20); // Compact history size
    lv_chart_set_div_line_count(s_detail_chart, 4, 6);
    lv_chart_set_range(s_detail_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100); // 0.0 to 10.0 ppm range
    
    // Premium transparent background
    lv_obj_set_style_bg_color(s_detail_chart, lv_color_hex(0x151C30), 0);
    lv_obj_set_style_bg_opa(s_detail_chart, LV_OPA_60, 0);
    lv_obj_set_style_border_color(s_detail_chart, lv_color_hex(0x283552), 0);
    lv_obj_set_style_border_width(s_detail_chart, 1, 0);
    lv_obj_set_style_radius(s_detail_chart, 12, 0);

    // Remove point dots (indicator) to save RAM and draw cycles
    lv_obj_set_style_width(s_detail_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_detail_chart, 0, LV_PART_INDICATOR);

    // Styled curve line (Sky Blue)
    lv_obj_set_style_line_width(s_detail_chart, 3, LV_PART_ITEMS);
    
    // Faint grid lines to avoid clutter
    lv_obj_set_style_line_color(s_detail_chart, lv_color_hex(0x1F2A44), LV_PART_MAIN);
    lv_obj_set_style_line_opa(s_detail_chart, LV_OPA_30, LV_PART_MAIN);

    s_chart_ser_voc = lv_chart_add_series(s_detail_chart, lv_color_hex(0x4AA3FF), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(s_detail_chart, s_chart_ser_voc, 0);



    /* --- RIGHT SIDE: Unified Control Panel (Width 300px) --- */
    lv_obj_t *ctrl_panel = lv_obj_create(detail_card);
    apply_no_shadow(ctrl_panel);
    lv_obj_set_size(ctrl_panel, 280, 420);
    lv_obj_set_pos(ctrl_panel, 580, 20);
    lv_obj_set_style_bg_color(ctrl_panel, lv_color_hex(0x141A2D), 0);
    lv_obj_set_style_border_color(ctrl_panel, lv_color_hex(0x283552), 0);
    lv_obj_set_style_border_width(ctrl_panel, 1, 0);
    lv_obj_set_style_radius(ctrl_panel, 20, 0);

    lv_obj_t *lbl_ctrl_title = lv_label_create(ctrl_panel);
    lv_label_set_text(lbl_ctrl_title, "DEVICE ACTUATORS");
    lv_obj_set_style_text_color(lbl_ctrl_title, lv_color_hex(0x8FA3B8), 0);
    lv_obj_set_style_text_font(lbl_ctrl_title, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_ctrl_title, LV_ALIGN_TOP_MID, 0, 15);

    // Warmup warning status label
    s_warmup_lbl = lv_label_create(ctrl_panel);
    lv_label_set_text(s_warmup_lbl, "");
    lv_obj_set_style_text_color(s_warmup_lbl, lv_color_hex(0xFFB84D), 0);
    lv_obj_set_style_text_font(s_warmup_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_warmup_lbl, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_add_flag(s_warmup_lbl, LV_OBJ_FLAG_HIDDEN);

    // 1. UV Switch Button
    s_uv_btn = lv_btn_create(ctrl_panel);
    lv_obj_set_size(s_uv_btn, 110, 42);
    lv_obj_align(s_uv_btn, LV_ALIGN_TOP_LEFT, 20, 55);
    lv_obj_set_style_radius(s_uv_btn, 21, 0);
    s_uv_lbl = lv_label_create(s_uv_btn);
    lv_label_set_text(s_uv_lbl, "UV OFF");
    lv_obj_set_style_text_color(s_uv_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_uv_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_uv_lbl);
    lv_obj_add_event_cb(s_uv_btn, [](lv_event_t * e) {
        bool cur_uv = false;
        if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            int di = s_current_device_idx;
            if (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) {
                cur_uv = s_device_apps[di].data.uv;
            } else {
                cur_uv = s_udp_node1.uv;
            }
            xSemaphoreGive(s_udp_data_mutex);
        }
        bool new_uv = !cur_uv;
        {
            int di = s_current_device_idx;
            const char *dn = (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) ? s_device_apps[di].name : "";
            uart_receiver_send_cmd(dn, new_uv ? "uv_on" : "uv_off");
        }
        ui_set_uv(new_uv);
    }, LV_EVENT_CLICKED, NULL);

    // UV Duration adjustments
    s_uv_remain_lbl = lv_label_create(ctrl_panel);
    lv_label_set_text(s_uv_remain_lbl, "UV remain: --");
    lv_obj_set_style_text_color(s_uv_remain_lbl, lv_color_hex(0x91A0B8), 0);
    lv_obj_set_style_text_font(s_uv_remain_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_uv_remain_lbl, LV_ALIGN_TOP_LEFT, 140, 55);

    s_uv_dur_lbl = lv_label_create(ctrl_panel);
    lv_label_set_text(s_uv_dur_lbl, "UV dur: --s");
    lv_obj_set_style_text_color(s_uv_dur_lbl, lv_color_hex(0x91A0B8), 0);
    lv_obj_set_style_text_font(s_uv_dur_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_uv_dur_lbl, LV_ALIGN_TOP_LEFT, 140, 75);

    // Duration adjusting buttons
    lv_obj_t * uv_dur_minus_btn = lv_btn_create(ctrl_panel);
    lv_obj_set_size(uv_dur_minus_btn, 110, 36);
    lv_obj_align(uv_dur_minus_btn, LV_ALIGN_TOP_LEFT, 20, 105);
    lv_obj_set_style_radius(uv_dur_minus_btn, 18, 0);
    lv_obj_set_style_bg_color(uv_dur_minus_btn, lv_color_hex(0x222B45), 0);
    lv_obj_set_style_border_color(uv_dur_minus_btn, lv_color_hex(0x3B4F7A), 0);
    lv_obj_set_style_border_width(uv_dur_minus_btn, 1, 0);
    lv_obj_t * uv_dur_minus_lbl = lv_label_create(uv_dur_minus_btn);
    lv_label_set_text(uv_dur_minus_lbl, "Dur -30s");
    lv_obj_set_style_text_color(uv_dur_minus_lbl, lv_color_hex(0xE6ECF5), 0);
    lv_obj_set_style_text_font(uv_dur_minus_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(uv_dur_minus_lbl);
    lv_obj_add_event_cb(uv_dur_minus_btn, [](lv_event_t * e) {
        int new_dur = s_uv_dur_setting - 30;
        if (new_dur < 30) new_dur = 30;
        s_uv_dur_setting = new_dur;
        if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_udp_node1.uv_dur = new_dur;
            int di = s_current_device_idx;
            if (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) {
                s_device_apps[di].data.uv_dur = new_dur;
            }
            xSemaphoreGive(s_udp_data_mutex);
        }
        if (s_uv_dur_lbl) lv_label_set_text_fmt(s_uv_dur_lbl, "UV dur: %ds", new_dur);
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "uv_dur_%d", new_dur);
        int di = s_current_device_idx;
        const char *dn = (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) ? s_device_apps[di].name : "";
        uart_receiver_send_cmd(dn, cmd);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * uv_dur_plus_btn = lv_btn_create(ctrl_panel);
    lv_obj_set_size(uv_dur_plus_btn, 110, 36);
    lv_obj_align(uv_dur_plus_btn, LV_ALIGN_TOP_LEFT, 145, 105);
    lv_obj_set_style_radius(uv_dur_plus_btn, 18, 0);
    lv_obj_set_style_bg_color(uv_dur_plus_btn, lv_color_hex(0x222B45), 0);
    lv_obj_set_style_border_color(uv_dur_plus_btn, lv_color_hex(0x3B4F7A), 0);
    lv_obj_set_style_border_width(uv_dur_plus_btn, 1, 0);
    lv_obj_t * uv_dur_plus_lbl = lv_label_create(uv_dur_plus_btn);
    lv_label_set_text(uv_dur_plus_lbl, "Dur +30s");
    lv_obj_set_style_text_color(uv_dur_plus_lbl, lv_color_hex(0xE6ECF5), 0);
    lv_obj_set_style_text_font(uv_dur_plus_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(uv_dur_plus_lbl);
    lv_obj_add_event_cb(uv_dur_plus_btn, [](lv_event_t * e) {
        int new_dur = s_uv_dur_setting + 30;
        if (new_dur > 600) new_dur = 600;
        s_uv_dur_setting = new_dur;
        if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_udp_node1.uv_dur = new_dur;
            int di = s_current_device_idx;
            if (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) {
                s_device_apps[di].data.uv_dur = new_dur;
            }
            xSemaphoreGive(s_udp_data_mutex);
        }
        if (s_uv_dur_lbl) lv_label_set_text_fmt(s_uv_dur_lbl, "UV dur: %ds", new_dur);
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "uv_dur_%d", new_dur);
        int di = s_current_device_idx;
        const char *dn = (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) ? s_device_apps[di].name : "";
        uart_receiver_send_cmd(dn, cmd);
    }, LV_EVENT_CLICKED, NULL);

    // 2. Fogger Control Button
    s_fog_btn = lv_btn_create(ctrl_panel);
    lv_obj_set_size(s_fog_btn, 235, 42);
    lv_obj_align(s_fog_btn, LV_ALIGN_TOP_LEFT, 20, 155);
    lv_obj_set_style_radius(s_fog_btn, 21, 0);
    s_fog_lbl = lv_label_create(s_fog_btn);
    lv_label_set_text(s_fog_lbl, "FOG OFF");
    lv_obj_set_style_text_color(s_fog_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_fog_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_fog_lbl);
    lv_obj_add_event_cb(s_fog_btn, [](lv_event_t * e) {
        bool cur_fog = false;
        if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            int di = s_current_device_idx;
            if (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) {
                cur_fog = s_device_apps[di].data.fog;
            } else {
                cur_fog = s_udp_node1.fog;
            }
            xSemaphoreGive(s_udp_data_mutex);
        }
        bool new_fog = !cur_fog;
        {
            int di = s_current_device_idx;
            const char *dn = (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) ? s_device_apps[di].name : "";
            uart_receiver_send_cmd(dn, new_fog ? "fog_on" : "fog_off");
        }
        ui_set_fog(new_fog);
    }, LV_EVENT_CLICKED, NULL);

    // 3. Fan Control Button
    s_fan_btn = lv_btn_create(ctrl_panel);
    lv_obj_set_size(s_fan_btn, 235, 42);
    lv_obj_align(s_fan_btn, LV_ALIGN_TOP_LEFT, 20, 205);
    lv_obj_set_style_radius(s_fan_btn, 21, 0);
    s_fan_lbl = lv_label_create(s_fan_btn);
    lv_label_set_text(s_fan_lbl, "FAN OFF");
    lv_obj_set_style_text_color(s_fan_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_fan_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_fan_lbl);
    lv_obj_add_event_cb(s_fan_btn, [](lv_event_t * e) {
        bool cur_fan = false;
        if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            int di = s_current_device_idx;
            if (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) {
                cur_fan = s_device_apps[di].data.fan;
            } else {
                cur_fan = s_udp_node1.fan;
            }
            xSemaphoreGive(s_udp_data_mutex);
        }
        bool new_fan = !cur_fan;
        {
            int di = s_current_device_idx;
            const char *dn = (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) ? s_device_apps[di].name : "";
            uart_receiver_send_cmd(dn, new_fan ? "fan_on" : "fan_off");
        }
        ui_set_fan(new_fan);
    }, LV_EVENT_CLICKED, NULL);

    // 4. Lid Control Button
    s_lid_btn = lv_btn_create(ctrl_panel);
    lv_obj_set_size(s_lid_btn, 235, 42);
    lv_obj_align(s_lid_btn, LV_ALIGN_TOP_LEFT, 20, 255);
    lv_obj_set_style_radius(s_lid_btn, 21, 0);
    s_lid_lbl = lv_label_create(s_lid_btn);
    lv_label_set_text(s_lid_lbl, "LID OFF");
    lv_obj_set_style_text_color(s_lid_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lid_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_lid_lbl);
    lv_obj_add_event_cb(s_lid_btn, [](lv_event_t * e) {
        bool cur_lid = false;
        if (s_udp_data_mutex && xSemaphoreTake(s_udp_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            int di = s_current_device_idx;
            if (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) {
                cur_lid = s_device_apps[di].data.lid;
            } else {
                cur_lid = s_udp_node1.lid;
            }
            xSemaphoreGive(s_udp_data_mutex);
        }
        bool new_lid = !cur_lid;
        {
            int di = s_current_device_idx;
            const char *dn = (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) ? s_device_apps[di].name : "";
            uart_receiver_send_cmd(dn, new_lid ? "lid_on" : "lid_off");
        }
        ui_set_lid(new_lid);
    }, LV_EVENT_CLICKED, NULL);

    // 5. Cloud AI Auto-Control Button
    s_cloud_ai_btn = lv_btn_create(ctrl_panel);
    ESP_LOGI("UI", "Initializing s_cloud_ai_btn inside ctrl_panel at Y=305");
    lv_obj_set_size(s_cloud_ai_btn, 235, 42);
    lv_obj_align(s_cloud_ai_btn, LV_ALIGN_TOP_LEFT, 20, 305);
    lv_obj_set_style_radius(s_cloud_ai_btn, 21, 0);
    
    s_cloud_ai_lbl = lv_label_create(s_cloud_ai_btn);
    lv_label_set_text(s_cloud_ai_lbl, g_cloud_ai_auto ? "AI AUTO: ON" : "AI AUTO: OFF");
    lv_obj_set_style_text_font(s_cloud_ai_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_cloud_ai_lbl);

    if (g_cloud_ai_auto) {
        lv_obj_set_style_bg_color(s_cloud_ai_btn, lv_color_hex(0x00A86B), 0); // Jade Green
        lv_obj_set_style_bg_opa(s_cloud_ai_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_cloud_ai_btn, lv_color_hex(0xA7F3D0), 0);
        lv_obj_set_style_border_width(s_cloud_ai_btn, 1, 0);
        lv_obj_set_style_text_color(s_cloud_ai_lbl, lv_color_hex(0xFFFFFF), 0);
    } else {
        lv_obj_set_style_bg_color(s_cloud_ai_btn, lv_color_hex(0x1F2A44), 0); // Actuator Slate Blue
        lv_obj_set_style_bg_opa(s_cloud_ai_btn, LV_OPA_60, 0);
        lv_obj_set_style_border_color(s_cloud_ai_btn, lv_color_hex(0x3B4F7A), 0);
        lv_obj_set_style_border_width(s_cloud_ai_btn, 1, 0);
        lv_obj_set_style_text_color(s_cloud_ai_lbl, lv_color_hex(0xFFFFFF), 0);
    }

    lv_obj_add_event_cb(s_cloud_ai_btn, [](lv_event_t * e) {
        g_cloud_ai_auto = !g_cloud_ai_auto;
        lv_async_call(cloud_ai_update_ui_cb, (void *)(intptr_t)g_cloud_ai_auto);
        ESP_LOGI("UI", "Cloud AI Auto-Control set to: %s", g_cloud_ai_auto ? "ON" : "OFF");
    }, LV_EVENT_CLICKED, NULL);

    // 6. System Action Buttons (Skip Warmup, Delete)
    lv_obj_t * skip_warmup_btn = lv_btn_create(ctrl_panel);
    lv_obj_set_size(skip_warmup_btn, 110, 42);
    lv_obj_align(skip_warmup_btn, LV_ALIGN_TOP_LEFT, 20, 360);
    lv_obj_set_style_radius(skip_warmup_btn, 21, 0);
    lv_obj_set_style_bg_color(skip_warmup_btn, lv_color_hex(0xFF7A00), 0); // Orange warning action
    lv_obj_t * skip_warmup_lbl = lv_label_create(skip_warmup_btn);
    lv_label_set_text(skip_warmup_lbl, "Skip Warmup");
    lv_obj_set_style_text_color(skip_warmup_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(skip_warmup_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(skip_warmup_lbl);
    lv_obj_add_event_cb(skip_warmup_btn, [](lv_event_t * e) {
        {
            int di = s_current_device_idx;
            const char *dn = (di >= 0 && di < MAX_DEVICE_APPS && s_device_apps[di].active) ? s_device_apps[di].name : "";
            uart_receiver_send_cmd(dn, "skip_warmup");
        }
        uart_receiver_set_warmup_remaining(0);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * delete_btn = lv_btn_create(ctrl_panel);
    lv_obj_set_size(delete_btn, 110, 42);
    lv_obj_align(delete_btn, LV_ALIGN_TOP_LEFT, 145, 360);
    lv_obj_set_style_radius(delete_btn, 21, 0);
    lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0xE53E3E), 0); // Crimson red
    lv_obj_t * delete_lbl = lv_label_create(delete_btn);
    lv_label_set_text(delete_lbl, "Delete");
    lv_obj_set_style_text_color(delete_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(delete_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(delete_lbl);
    lv_obj_add_event_cb(delete_btn, [](lv_event_t * e) {
        int di = s_current_device_idx;
        if (di >= 0) {
            device_app_delete(di);
        }
    }, LV_EVENT_CLICKED, NULL);

    /* Reload startup wallpaper right before showing screen (slot 0 — no longer shared) */
    if (sd_card_bg_is_ready() && s_current_startup_wallpaper_path[0]) {
        const lv_image_dsc_t *img = sd_card_bg_load_jpeg_slot(s_current_startup_wallpaper_path, 0);
        if (img && s_startup_wallpaper_img) {
            lv_image_set_src(s_startup_wallpaper_img, img);
            ESP_LOGI("UI", "Startup wallpaper refreshed for display");
        }
    }

    lv_screen_load(scr_startup);

    // 开机后自动触发一次后台 Wi-Fi 扫描，以在不阻塞界面的情况下进行“扫描匹配式自动重连”
    request_wifi_scan();
}

/* Web camera: JPEG pre-encoded in camera callback, HTTP handler just reads it */

bool ui_is_camera_available(void) {
    return web_jpeg_available();
}

bool ui_get_camera_frame(uint8_t **buf, uint32_t *w, uint32_t *h, size_t *len) {
    return web_jpeg_get_frame(buf, w, h, len);
}

void ui_show_ai_analysis(const char* result) {
    if (!result) return;
    ESP_LOGI("UI", "AI Analysis Notification: %s", result);
    esp_err_t lock_ret = esp_lv_adapter_lock(pdMS_TO_TICKS(500));
    if (lock_ret == ESP_OK) {
        if (siri_state_label) {
            lv_label_set_text(siri_state_label, result);
        }
        esp_lv_adapter_unlock();
    }
}

// 远程训练模态框实现
static void train_modal_close_cb(lv_event_t * e) {
    LV_UNUSED(e);
    if (train_modal) {
        close_modal_with_zoom_anim_hidden(train_modal);
        cloud_sync_stop_train_feed_poll();
        if (train_model_name_ta) {
            lv_obj_clear_state(train_model_name_ta, LV_STATE_FOCUSED);
        }
    }
}

static void btn_start_train_cb(lv_event_t * e) {
    LV_UNUSED(e);
    if (!train_csv_list) return;

    const char *selected_files[MAX_CSV_FILES];
    int count = 0;

    uint32_t child_count = lv_obj_get_child_cnt(train_csv_list);
    for (uint32_t i = 0; i < child_count && count < MAX_CSV_FILES; i++) {
        lv_obj_t * child = lv_obj_get_child(train_csv_list, i);
        if (child && lv_obj_has_state(child, LV_STATE_CHECKED)) {
            const char *filename = (const char *)lv_obj_get_user_data(child);
            if (filename) {
                selected_files[count++] = filename;
            }
        }
    }

    if (count == 0) {
        ui_show_ai_analysis("Aborted: No CSV files selected.");
        return;
    }

    const char *model_name = lv_textarea_get_text(train_model_name_ta);
    if (strlen(model_name) == 0) {
        model_name = "p4_custom";
    }

    int target_accuracy = lv_slider_get_value(train_accuracy_slider);

    // Transition UI to progress view immediately
    g_training_progress = 0;

    cloud_sync_start_remote_training(selected_files, count, model_name, target_accuracy);
}

static void render_csv_list_ui(void) {
    if (!train_csv_list) return;
    lv_obj_clean(train_csv_list);

    if (g_csv_fetch_state == 3) {
        lv_obj_t * lbl = lv_label_create(train_csv_list);
        lv_label_set_text(lbl, "Failed to load CSV files.");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF4D4D), 0);
        lv_obj_center(lbl);
        return;
    }

    if (g_csv_file_count == 0) {
        lv_obj_t * lbl = lv_label_create(train_csv_list);
        lv_label_set_text(lbl, "No CSV files available.");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x91A0B8), 0);
        lv_obj_center(lbl);
        return;
    }

    for (int i = 0; i < g_csv_file_count; i++) {
        lv_obj_t * cb = lv_checkbox_create(train_csv_list);
        char buf[192];
        snprintf(buf, sizeof(buf), "%s (%d samples)", g_csv_filenames[i], g_csv_file_samples[i]);
        lv_checkbox_set_text(cb, buf);
        lv_obj_set_pos(cb, 10, i * 35);
        lv_obj_set_user_data(cb, (void *)g_csv_filenames[i]);
        lv_obj_set_style_text_color(cb, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(cb, &s_font_cjk_fallback, 0);
    }
}

extern "C" void ui_trigger_csv_list_render(void) {
    lv_async_call([](void * data) {
        LV_UNUSED(data);
        render_csv_list_ui();
    }, NULL);
}

static void create_training_modal(void) {
    lv_obj_t * overlay_parent = lv_layer_top();

    train_modal = lv_obj_create(overlay_parent);
    apply_no_shadow(train_modal);
    lv_obj_set_size(train_modal, 600, 480);
    lv_obj_align(train_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(train_modal, lv_color_hex(0x1B2238), 0);
    lv_obj_set_style_bg_opa(train_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(train_modal, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(train_modal, 2, 0);
    lv_obj_set_style_radius(train_modal, 16, 0);
    lv_obj_add_flag(train_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(train_modal);
    lv_label_set_text(title, "Remote AI Model Training (远程AI模型训练)");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &s_font_cjk_fallback, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t * close_btn = lv_btn_create(train_modal);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xFF4D4D), 0);
    lv_obj_set_style_radius(close_btn, 18, 0);
    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, train_modal_close_cb, LV_EVENT_CLICKED, NULL);

    train_list_title = lv_label_create(train_modal);
    lv_label_set_text(train_list_title, "Select CSV Datasets:");
    lv_obj_set_style_text_color(train_list_title, lv_color_hex(0x91A0B8), 0);
    lv_obj_set_style_text_font(train_list_title, &s_font_cjk_fallback, 0);
    lv_obj_align(train_list_title, LV_ALIGN_TOP_LEFT, 20, 50);

    train_csv_list = lv_obj_create(train_modal);
    lv_obj_set_size(train_csv_list, 320, 340);
    lv_obj_align(train_csv_list, LV_ALIGN_TOP_LEFT, 20, 80);
    lv_obj_set_style_bg_color(train_csv_list, lv_color_hex(0x131828), 0);
    lv_obj_set_style_border_color(train_csv_list, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(train_csv_list, 1, 0);
    lv_obj_set_scroll_dir(train_csv_list, LV_DIR_VER);

    train_name_lbl = lv_label_create(train_modal);
    lv_label_set_text(train_name_lbl, "Model Name:");
    lv_obj_set_style_text_color(train_name_lbl, lv_color_hex(0x91A0B8), 0);
    lv_obj_set_style_text_font(train_name_lbl, &s_font_cjk_fallback, 0);
    lv_obj_align(train_name_lbl, LV_ALIGN_TOP_LEFT, 360, 50);

    train_model_name_ta = lv_textarea_create(train_modal);
    lv_textarea_set_one_line(train_model_name_ta, true);
    lv_textarea_set_text(train_model_name_ta, "p4_custom");
    lv_obj_set_size(train_model_name_ta, 200, 40);
    lv_obj_align(train_model_name_ta, LV_ALIGN_TOP_LEFT, 360, 80);
    lv_obj_set_style_bg_color(train_model_name_ta, lv_color_hex(0x131828), 0);
    lv_obj_set_style_text_color(train_model_name_ta, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(train_model_name_ta, lv_color_hex(0x2A3A5A), 0);

    // Create a keyboard associated with the model name text area
    lv_obj_t * kb = lv_keyboard_create(train_modal);
    lv_obj_set_size(kb, 560, 180);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_keyboard_set_textarea(kb, train_model_name_ta);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(train_model_name_ta, [](lv_event_t * e) {
        lv_event_code_t code = lv_event_get_code(e);
        lv_obj_t * ta = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_t * keyboard = (lv_obj_t *)lv_event_get_user_data(e);
        if (code == LV_EVENT_FOCUSED) {
            lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(keyboard);
        } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY) {
            lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        }
    }, LV_EVENT_ALL, kb);

    train_acc_lbl = lv_label_create(train_modal);
    lv_label_set_text(train_acc_lbl, "Target Accuracy:");
    lv_obj_set_style_text_color(train_acc_lbl, lv_color_hex(0x91A0B8), 0);
    lv_obj_set_style_text_font(train_acc_lbl, &s_font_cjk_fallback, 0);
    lv_obj_align(train_acc_lbl, LV_ALIGN_TOP_LEFT, 360, 140);

    train_accuracy_label = lv_label_create(train_modal);
    lv_label_set_text(train_accuracy_label, "85%");
    lv_obj_set_style_text_color(train_accuracy_label, lv_color_hex(0x63B3ED), 0);
    lv_obj_set_style_text_font(train_accuracy_label, &lv_font_montserrat_14, 0);
    lv_obj_align(train_accuracy_label, LV_ALIGN_TOP_LEFT, 500, 140);

    train_accuracy_slider = lv_slider_create(train_modal);
    lv_slider_set_range(train_accuracy_slider, 50, 100);
    lv_slider_set_value(train_accuracy_slider, 85, LV_ANIM_OFF);
    lv_obj_set_size(train_accuracy_slider, 200, 15);
    lv_obj_align(train_accuracy_slider, LV_ALIGN_TOP_LEFT, 360, 170);
    lv_obj_set_style_bg_color(train_accuracy_slider, lv_color_hex(0x131828), LV_PART_MAIN);
    lv_obj_set_style_bg_color(train_accuracy_slider, lv_color_hex(0x63B3ED), LV_PART_INDICATOR);
    
    lv_obj_add_event_cb(train_accuracy_slider, [](lv_event_t * e) {
        lv_obj_t * slider = (lv_obj_t *)lv_event_get_target(e);
        int val = lv_slider_get_value(slider);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(train_accuracy_label, buf);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    btn_start_train = lv_btn_create(train_modal);
    lv_obj_set_size(btn_start_train, 200, 48);
    lv_obj_align(btn_start_train, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_bg_color(btn_start_train, lv_color_hex(0x39D98A), 0);
    lv_obj_set_style_radius(btn_start_train, 24, 0);

    lv_obj_t * btn_lbl = lv_label_create(btn_start_train);
    lv_label_set_text(btn_lbl, "Start Training");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0x131828), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);

    lv_obj_add_event_cb(btn_start_train, btn_start_train_cb, LV_EVENT_CLICKED, NULL);

    /* Progress Container hosting all progress detail widgets */
    train_progress_cont = lv_obj_create(train_modal);
    lv_obj_set_size(train_progress_cont, 560, 380);
    lv_obj_align(train_progress_cont, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(train_progress_cont, lv_color_hex(0x131828), 0);
    lv_obj_set_style_border_color(train_progress_cont, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_border_width(train_progress_cont, 1, 0);
    lv_obj_set_style_radius(train_progress_cont, 12, 0);
    lv_obj_add_flag(train_progress_cont, LV_OBJ_FLAG_HIDDEN);

    modal_model_name_val = lv_label_create(train_progress_cont);
    lv_label_set_text(modal_model_name_val, "Model Name: --");
    lv_obj_set_style_text_color(modal_model_name_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(modal_model_name_val, &s_font_cjk_fallback, 0);
    lv_obj_align(modal_model_name_val, LV_ALIGN_TOP_LEFT, 20, 20);

    modal_status_badge = lv_label_create(train_progress_cont);
    lv_label_set_text(modal_status_badge, "IDLE");
    lv_obj_set_style_text_color(modal_status_badge, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(modal_status_badge, &s_font_cjk_fallback, 0);
    lv_obj_set_style_bg_color(modal_status_badge, lv_color_hex(0x4A5568), 0);
    lv_obj_set_style_bg_opa(modal_status_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(modal_status_badge, 8, 0);
    lv_obj_set_style_pad_ver(modal_status_badge, 4, 0);
    lv_obj_set_style_radius(modal_status_badge, 6, 0);
    lv_obj_align(modal_status_badge, LV_ALIGN_TOP_RIGHT, -20, 15);

    modal_phase_val = lv_label_create(train_progress_cont);
    lv_label_set_text(modal_phase_val, "Phase: Preparing...");
    lv_obj_set_style_text_color(modal_phase_val, lv_color_hex(0x91A0B8), 0);
    lv_obj_set_style_text_font(modal_phase_val, &s_font_cjk_fallback, 0);
    lv_obj_align(modal_phase_val, LV_ALIGN_TOP_LEFT, 20, 65);

    modal_training_bar = lv_bar_create(train_progress_cont);
    lv_obj_set_size(modal_training_bar, 420, 20);
    lv_obj_align(modal_training_bar, LV_ALIGN_TOP_LEFT, 20, 115);
    lv_obj_set_style_bg_color(modal_training_bar, lv_color_hex(0x1A2238), LV_PART_MAIN);
    lv_obj_set_style_bg_color(modal_training_bar, lv_color_hex(0x63B3ED), LV_PART_INDICATOR);
    lv_obj_set_style_radius(modal_training_bar, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(modal_training_bar, 10, LV_PART_INDICATOR);

    modal_training_label = lv_label_create(train_progress_cont);
    lv_label_set_text(modal_training_label, "0%");
    lv_obj_set_style_text_color(modal_training_label, lv_color_hex(0x63B3ED), 0);
    lv_obj_set_style_text_font(modal_training_label, &lv_font_montserrat_16, 0);
    lv_obj_align(modal_training_label, LV_ALIGN_TOP_RIGHT, -20, 115);

    modal_epoch_val = lv_label_create(train_progress_cont);
    lv_label_set_text(modal_epoch_val, "Epoch: -- / --");
    lv_obj_set_style_text_color(modal_epoch_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(modal_epoch_val, &s_font_cjk_fallback, 0);
    lv_obj_align(modal_epoch_val, LV_ALIGN_TOP_LEFT, 20, 160);

    modal_accuracy_val = lv_label_create(train_progress_cont);
    lv_label_set_text(modal_accuracy_val, "Accuracy: --");
    lv_obj_set_style_text_color(modal_accuracy_val, lv_color_hex(0x39D98A), 0);
    lv_obj_set_style_text_font(modal_accuracy_val, &s_font_cjk_fallback, 0);
    lv_obj_align(modal_accuracy_val, LV_ALIGN_TOP_LEFT, 20, 205);

    modal_loss_val = lv_label_create(train_progress_cont);
    lv_label_set_text(modal_loss_val, "Loss: --");
    lv_obj_set_style_text_color(modal_loss_val, lv_color_hex(0xFF4D4D), 0);
    lv_obj_set_style_text_font(modal_loss_val, &s_font_cjk_fallback, 0);
    lv_obj_align(modal_loss_val, LV_ALIGN_TOP_LEFT, 20, 250);

    modal_training_status = lv_label_create(train_progress_cont);
    lv_label_set_text(modal_training_status, "");
    lv_obj_set_style_text_color(modal_training_status, lv_color_hex(0xFFC107), 0);
    lv_obj_set_style_text_font(modal_training_status, &s_font_cjk_fallback, 0);
    lv_obj_set_width(modal_training_status, 520);
    lv_obj_align(modal_training_status, LV_ALIGN_TOP_LEFT, 20, 295);
}

static void open_training_modal_cb(lv_event_t * e) {
    LV_UNUSED(e);
    if (!train_modal) {
        create_training_modal();
    }
    
    lv_obj_move_foreground(train_modal);
    lv_obj_remove_flag(train_modal, LV_OBJ_FLAG_HIDDEN);
    open_modal_with_zoom_anim(train_modal);
    
    cloud_sync_start_train_feed_poll();
    
    if (g_training_progress < 0) {
        lv_obj_clean(train_csv_list);
        lv_obj_t * loading_lbl = lv_label_create(train_csv_list);
        lv_label_set_text(loading_lbl, "Fetching CSV list from server...");
        lv_obj_set_style_text_color(loading_lbl, lv_color_hex(0x91A0B8), 0);
        lv_obj_set_style_text_font(loading_lbl, &s_font_cjk_fallback, 0);
        lv_obj_center(loading_lbl);
        
        cloud_sync_fetch_csv_list();
    }
}

void ui_update_qr_pairing_result(const char *name, const char *mac, bool success)
{
    if (esp_lv_adapter_lock(pdMS_TO_TICKS(100)) == ESP_OK) {
        if (qr_result_label && qr_result_panel) {
            char info[256];
            if (success) {
                if (mac && mac[0])
                    snprintf(info, sizeof(info), "Pairing Success!\nDevice: %s\nMAC: %s", name, mac);
                else
                    snprintf(info, sizeof(info), "C6 Confirmed\nDevice: %s", name);
                lv_obj_set_style_bg_color(qr_result_panel, lv_color_hex(0x005500), 0);
            } else {
                snprintf(info, sizeof(info), "Pairing Failed!\nDevice: %s", name);
                lv_obj_set_style_bg_color(qr_result_panel, lv_color_hex(0x550000), 0);
            }
            lv_label_set_text(qr_result_label, info);
            lv_obj_remove_flag(qr_result_panel, LV_OBJ_FLAG_HIDDEN);
        }
        esp_lv_adapter_unlock();
    }
}
