/*
 * SD Card Background Image Loader for LVGL
 * Uses ESP32-P4 hardware JPEG decoder to load images from SD card
 */
#include "sd_card_bg.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "SD_BG";

static bool s_sd_ready = false;

/* Two independent slots so startup and main wallpapers don't share a buffer */
#define BG_SLOT_COUNT 2

typedef struct {
    lv_image_dsc_t dsc;
    uint8_t *buf;
    size_t buf_size;
} bg_slot_t;

static bg_slot_t s_slots[BG_SLOT_COUNT] = {
    { .dsc = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565 }, .data = NULL }, .buf = NULL, .buf_size = 0 },
    { .dsc = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565 }, .data = NULL }, .buf = NULL, .buf_size = 0 },
};

esp_err_t sd_card_bg_init(void)
{
    if (s_sd_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SD card...");
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_sd_ready = true;
    ESP_LOGI(TAG, "SD card mounted at %s", CONFIG_BSP_SD_MOUNT_POINT);
    return ESP_OK;
}

bool sd_card_bg_is_ready(void)
{
    return s_sd_ready;
}

/*
 * Decode JPEG file to RGB565 buffer using ESP32-P4 hardware decoder
 */
static uint8_t *jpeg_decode_to_rgb565(const char *filepath, int *out_w, int *out_h, size_t *out_size)
{
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open: %s", filepath);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 10 * 1024 * 1024) {
        ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
        fclose(fp);
        return NULL;
    }

    /* Read JPEG data into PSRAM */
    uint8_t *jpeg_data = (uint8_t *)heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM);
    if (!jpeg_data) {
        ESP_LOGE(TAG, "Failed to alloc JPEG input buffer");
        fclose(fp);
        return NULL;
    }

    size_t read_bytes = fread(jpeg_data, 1, (size_t)file_size, fp);
    fclose(fp);

    if ((long)read_bytes != file_size) {
        ESP_LOGE(TAG, "Incomplete read: %zu/%ld", read_bytes, file_size);
        free(jpeg_data);
        return NULL;
    }

    /* Get image info */
    jpeg_decode_picture_info_t pic_info = {};
    esp_err_t ret = jpeg_decoder_get_info(jpeg_data, (uint32_t)file_size, &pic_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_decoder_get_info failed: %s", esp_err_to_name(ret));
        free(jpeg_data);
        return NULL;
    }

    int width = (int)pic_info.width;
    int height = (int)pic_info.height;
    ESP_LOGI(TAG, "JPEG: %dx%d", width, height);

    /* JPEG output is 16-aligned */
    int aligned_w = ((width + 15) / 16) * 16;
    int aligned_h = ((height + 15) / 16) * 16;
    size_t rgb565_size = (size_t)(aligned_w * aligned_h * 2);

    /* Create HW decoder first so we can use jpeg_alloc_decoder_mem */
    jpeg_decode_engine_cfg_t dec_eng_cfg = {
        .intr_priority = 1,
        .timeout_ms = 100,
    };
    jpeg_decoder_handle_t jpeg_dec = NULL;
    ret = jpeg_new_decoder_engine(&dec_eng_cfg, &jpeg_dec);
    if (ret != ESP_OK || !jpeg_dec) {
        ESP_LOGE(TAG, "jpeg_new_decoder_engine failed");
        free(jpeg_data);
        return NULL;
    }

    /* Allocate output buffer using jpeg_alloc_decoder_mem for proper alignment */
    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated_size = 0;
    uint8_t *rgb565_buf = (uint8_t *)jpeg_alloc_decoder_mem(rgb565_size, &mem_cfg, &allocated_size);
    if (!rgb565_buf) {
        ESP_LOGE(TAG, "Failed to alloc RGB565 buffer (%zu bytes)", rgb565_size);
        jpeg_del_decoder_engine(jpeg_dec);
        free(jpeg_data);
        return NULL;
    }

    /* Decode */
    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT709,
    };
    uint32_t decoded_size = 0;
    ret = jpeg_decoder_process(jpeg_dec, &decode_cfg,
                               jpeg_data, (uint32_t)file_size,
                               rgb565_buf, (uint32_t)rgb565_size,
                               &decoded_size);

    jpeg_del_decoder_engine(jpeg_dec);
    free(jpeg_data);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_decoder_process failed: %s", esp_err_to_name(ret));
        heap_caps_free(rgb565_buf);
        return NULL;
    }

    if (decoded_size > rgb565_size) {
        ESP_LOGW(TAG, "Decoder wrote %" PRIu32 " bytes, buffer is %zu bytes (possible overflow)", decoded_size, rgb565_size);
    }

    if (out_w) *out_w = width;
    if (out_h) *out_h = height;
    if (out_size) *out_size = rgb565_size;

    return rgb565_buf;
}

const lv_image_dsc_t *sd_card_bg_load_jpeg_slot(const char *file_path, int slot)
{
    if (!s_sd_ready) {
        ESP_LOGE(TAG, "SD card not ready");
        return NULL;
    }
    if (slot < 0 || slot >= BG_SLOT_COUNT) {
        ESP_LOGE(TAG, "Invalid slot %d", slot);
        return NULL;
    }

    bg_slot_t *s = &s_slots[slot];

    int width = 0, height = 0;
    size_t buf_size = 0;

    uint8_t *new_buf = jpeg_decode_to_rgb565(file_path, &width, &height, &buf_size);
    if (!new_buf) {
        return NULL;
    }

    /* Swap: install new buffer, then free old */
    uint8_t *old_buf = s->buf;
    s->buf = new_buf;
    s->buf_size = buf_size;

    s->dsc.header.w = (lv_coord_t)width;
    s->dsc.header.h = (lv_coord_t)height;
    s->dsc.header.stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
    s->dsc.data = s->buf;
    s->dsc.data_size = buf_size;

    if (old_buf) {
        heap_caps_free(old_buf);
    }

    ESP_LOGI(TAG, "Slot %d loaded: %dx%d, stride=%d", slot, width, height, s->dsc.header.stride);
    return &s->dsc;
}

/* Backward-compatible wrapper — uses slot 0 */
const lv_image_dsc_t *sd_card_bg_load_jpeg(const char *file_path)
{
    return sd_card_bg_load_jpeg_slot(file_path, 0);
}

lv_obj_t *sd_card_bg_set(lv_obj_t *parent, const lv_image_dsc_t *img_dsc)
{
    if (!parent || !img_dsc) {
        return NULL;
    }

    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, img_dsc);
    lv_obj_set_size(img, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_set_style_pad_all(img, 0, 0);

    return img;
}

lv_obj_t *sd_card_bg_set_from_file(lv_obj_t *parent, const char *file_path)
{
    const lv_image_dsc_t *img_dsc = sd_card_bg_load_jpeg(file_path);
    if (!img_dsc) {
        return NULL;
    }
    return sd_card_bg_set(parent, img_dsc);
}
