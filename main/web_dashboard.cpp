#include "web_dashboard.h"
#include "ui.h"
#include "app_system.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_cache.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "WEB_DASH";

extern float g_temperature;
extern float g_humidity;
extern float g_pressure;

extern "C" {
    void ui_get_node1_data(ui_sensor_data_t *out);
    void ui_get_node2_data(ui_sensor_data_t *out);
    const char* ui_get_ai_result(void);
    const char* ui_get_ai_history_json(void);
}

/* ============================================================
 * JPEG Pre-encode Engine
 * - Encodes JPEG in camera frame callback (video task context)
 * - Double-buffer + spinlock for lock-free handoff to HTTP
 * - HTTP handler just copies pre-encoded data (zero encode wait)
 * ============================================================ */
#define WEB_CAM_W 256
#define WEB_CAM_H 144  /* 16:9, both multiple of 16 for JPEG 420 */

#define JPEG_BUF_SIZE 49152   /* 48KB max, enough for 256x144 q=50 */
#define JPEG_BUF_COUNT 2     /* double-buffer */

/* PPA requires PSRAM buffers aligned to L1+L2 cache line size (128 bytes on ESP32-P4) */
#define PPA_BUF_ALIGNMENT 128

static uint8_t *s_rgb888_buf = NULL;
static size_t   s_rgb888_buf_size = 0;
static uint8_t *s_jpeg_bufs[JPEG_BUF_COUNT] = {NULL};
static size_t   s_jpeg_sizes[JPEG_BUF_COUNT] = {0};
static int      s_jpeg_write_idx = 0;   /* which buffer encoder writes to */
static volatile int s_jpeg_ready_idx = -1;  /* which buffer has complete JPEG (-1=none) */
static volatile int s_jpeg_lock = 0;   /* spinlock for write_idx swap */
static jpeg_enc_handle_t s_jpeg_enc = NULL;
static int s_web_frame_skip = 0;
static bool s_use_ppa = false;
/* Temporarily disable PPA for web pipeline to debug screen blank issue.
 * Set to true to re-enable PPA hardware acceleration. */
static bool s_ppa_enabled_for_web = true;

/* Called from camera_frame_cb (video stream task) */
extern "C" void web_jpeg_encode_frame(const uint8_t *cam_buf, uint32_t w, uint32_t h)
{
    if (cam_buf == NULL) return;

    /* Encode every 3rd frame to reduce CPU load (camera runs ~15fps, web needs ~5fps) */
    if (++s_web_frame_skip % 3 != 0) return;

    size_t rgb888_size = WEB_CAM_W * WEB_CAM_H * 3;

    /* One-time init */
    static bool s_jpeg_init_done = false;
    if (!s_jpeg_init_done) {
        s_rgb888_buf_size = rgb888_size;
        s_rgb888_buf = (uint8_t *)heap_caps_aligned_alloc(PPA_BUF_ALIGNMENT, rgb888_size, MALLOC_CAP_SPIRAM);
        if (!s_rgb888_buf) return;
        for (int i = 0; i < JPEG_BUF_COUNT; i++) {
            s_jpeg_bufs[i] = (uint8_t *)heap_caps_aligned_alloc(PPA_BUF_ALIGNMENT, JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);
            if (!s_jpeg_bufs[i]) return;
        }
        jpeg_enc_config_t enc_cfg = {
            .width       = WEB_CAM_W,
            .height      = WEB_CAM_H,
            .src_type    = JPEG_PIXEL_FORMAT_RGB888,
            .subsampling = JPEG_SUBSAMPLE_420,
            .quality     = 50,
            .rotate      = JPEG_ROTATE_0D,
            .task_enable = false,
        };
        if (jpeg_enc_open(&enc_cfg, &s_jpeg_enc) != JPEG_ERR_OK) return;

        /* Try PPA hardware acceleration for RGB565→RGB888 + downscale */
        ppa_client_handle_t ppa_client = app_ppa_get_web_client();
        if (ppa_client != NULL && s_ppa_enabled_for_web) {
            s_use_ppa = true;
            ESP_LOGI("WEB_DASH", "PPA hardware acceleration ENABLED for web JPEG pipeline");
        } else {
            ESP_LOGW("WEB_DASH", "PPA not available or disabled, using software RGB565→RGB888 fallback");
        }

        s_jpeg_init_done = true;
    }

    /* ============================================================
     * Stage 1: RGB565 camera frame → RGB888 downscaled buffer
     * Use PPA hardware if available, else software fallback
     * ============================================================ */
    if (s_use_ppa) {
        ppa_client_handle_t ppa_client = app_ppa_get_web_client();
        ppa_srm_oper_config_t srm_cfg = {
            .in = {
                .buffer = cam_buf,
                .pic_w = w,
                .pic_h = h,
                .block_w = w,
                .block_h = h,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = s_rgb888_buf,
                .buffer_size = s_rgb888_buf_size,
                .pic_w = WEB_CAM_W,
                .pic_h = WEB_CAM_H,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = (float)WEB_CAM_W / (float)w,
            .scale_y = (float)WEB_CAM_H / (float)h,
            .mirror_x = false,
            .mirror_y = false,
            .rgb_swap = true,    /* PPA outputs BGR888 by default; swap to RGB888 for JPEG */
            .byte_swap = false,
            .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
            .mode = PPA_TRANS_MODE_BLOCKING,
            .user_data = NULL,
        };
        esp_err_t ppa_ret = ppa_do_scale_rotate_mirror(ppa_client, &srm_cfg);
        if (ppa_ret != ESP_OK) {
            ESP_LOGW("WEB_DASH", "PPA SRM failed: %s, falling back to SW", esp_err_to_name(ppa_ret));
            s_use_ppa = false;  /* Fallback permanently if PPA fails */
        }
    }

    if (!s_use_ppa) {
        /* Software fallback: fixed-point stepping downscale + RGB565→RGB888 */
        static uint32_t s_src_w = 0, s_src_h = 0;
        static int s_src_row_ofs[WEB_CAM_H];
        static int32_t s_x_step;

        if (s_src_w != w || s_src_h != h) {
            for (int y = 0; y < WEB_CAM_H; y++)
                s_src_row_ofs[y] = (y * h / WEB_CAM_H) * w;
            s_x_step = ((int32_t)w << 16) / WEB_CAM_W;
            s_src_w = w;
            s_src_h = h;
        }

        const uint16_t *src = (const uint16_t *)cam_buf;
        uint8_t *dst = s_rgb888_buf;
        for (int y = 0; y < WEB_CAM_H; y++) {
            const uint16_t *src_row = src + s_src_row_ofs[y];
            int32_t x_fp = 0;
            for (int x = 0; x < WEB_CAM_W; x++) {
                uint16_t px = src_row[x_fp >> 16];
                uint8_t r5 = (px >> 11) & 0x1F;
                uint8_t g6 = (px >> 5) & 0x3F;
                uint8_t b5 = px & 0x1F;
                dst[0] = (r5 << 3) | (r5 >> 2);
                dst[1] = (g6 << 2) | (g6 >> 4);
                dst[2] = (b5 << 3) | (b5 >> 2);
                dst += 3;
                x_fp += s_x_step;
            }
        }
    }

    /* ============================================================
     * Stage 2: JPEG encode RGB888 → JPEG (still CPU, but lightweight)
     * ============================================================ */
    int out_size = 0;
    int write_idx = s_jpeg_write_idx;
    jpeg_error_t ret = jpeg_enc_process(s_jpeg_enc, s_rgb888_buf, rgb888_size,
                                         s_jpeg_bufs[write_idx], JPEG_BUF_SIZE, &out_size);
    if (ret != JPEG_ERR_OK || out_size <= 0) return;

    s_jpeg_sizes[write_idx] = (size_t)out_size;

    /* Swap: publish new buffer, take old one for next write */
    while (__atomic_test_and_set(&s_jpeg_lock, __ATOMIC_ACQUIRE)) {}
    s_jpeg_ready_idx = write_idx;
    s_jpeg_write_idx = (write_idx + 1) % JPEG_BUF_COUNT;
    __atomic_clear(&s_jpeg_lock, __ATOMIC_RELEASE);
}

extern "C" bool web_jpeg_available(void)
{
    return (s_jpeg_ready_idx >= 0);
}

extern "C" bool web_jpeg_get_frame(uint8_t **buf, uint32_t *w, uint32_t *h, size_t *len)
{
    while (__atomic_test_and_set(&s_jpeg_lock, __ATOMIC_ACQUIRE)) {}
    int ready = s_jpeg_ready_idx;
    size_t size = (ready >= 0) ? s_jpeg_sizes[ready] : 0;
    __atomic_clear(&s_jpeg_lock, __ATOMIC_RELEASE);

    if (ready < 0 || size == 0) return false;

    *buf = s_jpeg_bufs[ready];
    *w = WEB_CAM_W;
    *h = WEB_CAM_H;
    *len = size;
    return true;
}

/* ============================================================ */

static const char *HTML_PAGE = R"(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>E-Nose Dashboard</title>
<style>
  *{margin:0;padding:0;box-sizing:border-box}
  body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:#0B1020;color:#E6ECF5;min-height:100vh;padding:20px}
  h1{text-align:center;color:#4AA3FF;margin-bottom:24px;font-size:28px}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px;max-width:1200px;margin:0 auto}
  .card{background:#151C30;border-radius:16px;padding:20px;border:1px solid #26324A}
  .card h2{font-size:16px;color:#4AA3FF;margin-bottom:12px;padding-bottom:8px;border-bottom:1px solid #26324A}
  .card.env h2{color:#39D98A}
  .metric{display:flex;justify-content:space-between;padding:6px 0;font-size:14px}
  .metric .label{color:#91A0B8}
  .metric .value{color:#E6ECF5;font-weight:600}
  .ai-box{grid-column:1/-1;background:#10182A;border:1px solid #4AA3FF}
  .ai-box pre{background:#0B1020;border-radius:8px;padding:16px;font-size:13px;line-height:1.6;white-space:pre-wrap;word-wrap:break-word;color:#E6ECF5;max-height:400px;overflow-y:auto;margin-top:8px}
  .status-dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
  .status-dot.green{background:#39D98A}
  .status-dot.yellow{background:#FFB84D}
  .status-dot.red{background:#FF5C5C}
  .footer{text-align:center;margin-top:24px;color:#91A0B8;font-size:12px}
  .env-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px}
  .env-item{text-align:center;padding:8px;background:#0B1020;border-radius:8px}
  .env-item .val{font-size:22px;font-weight:700;color:#39D98A}
  .env-item .lbl{font-size:11px;color:#91A0B8;margin-top:4px}
  .nodata{color:#91A0B8;font-style:italic;text-align:center;padding:16px 0}
  #camImg{width:100%;max-height:480px;background:#000;border-radius:8px;object-fit:contain;display:block}
</style>
</head>
<body>
<h1>🔬 E-Nose Sensor Dashboard</h1>
<div class="grid" id="dataGrid"></div>
<div class="grid" style="margin-top:16px;max-width:1200px;margin-left:auto;margin-right:auto">
  <div class="card" style="grid-column:1/-1">
    <h2>📷 Camera <span id="fps" style="font-size:12px;color:#91A0B8;font-weight:400"></span></h2>
    <img id="camImg" src="" alt="Camera">
  </div>
  <div class="card ai-box" style="grid-column:1/-1">
    <h2>🤖 AI Analysis</h2>
    <div id="aiContent"><div class="nodata">No analysis yet. Press "AI Analysis" on the P4 screen.</div></div>
  </div>
  <div class="card" style="grid-column:1/-1;max-height:500px;overflow-y:auto" id="historyCard">
    <h2>📜 AI History <span id="historyCount" style="font-size:12px;color:#91A0B8;font-weight:400"></span></h2>
    <div id="historyContent"><div class="nodata">Loading history...</div></div>
  </div>
</div>
<div class="footer">P4 Gateway &bull; Data refresh 3s &bull; Camera streaming &bull; <a href="#" onclick="loadHistory();return false" style="color:#4AA3FF">Refresh History</a></div>
<script>
function fmt(v,f){return v!==undefined&&v!==null?v.toFixed(f):'--'}
function escapeHtml(t){const d=document.createElement('div');d.appendChild(document.createTextNode(t));return d.innerHTML}
function renderData(data){
  const app=document.getElementById('dataGrid');
  const env=data.environment||{};
  const n1=data.node1||{},n2=data.node2||{};
  app.innerHTML=`
    <div class="card env">
      <h2>🌡 Environment</h2>
      <div class="env-grid">
        <div class="env-item"><div class="val">${fmt(env.temp,1)}&deg;C</div><div class="lbl">Temperature</div></div>
        <div class="env-item"><div class="val">${fmt(env.humidity,1)}%</div><div class="lbl">Humidity</div></div>
        <div class="env-item"><div class="val">${env.pressure!==undefined?Math.round(env.pressure):'--'}</div><div class="lbl">Pressure hPa</div></div>
      </div>
    </div>
    <div class="card">
      <h2><span class="status-dot ${n1.valid?'green':'yellow'}"></span>Node S3-A</h2>
      ${n1.valid?`
        <div class="metric"><span class="label">VOC</span><span class="value">${fmt(n1.voc,1)} ppm</span></div>
        <div class="metric"><span class="label">CO₂</span><span class="value">${n1.co2||'--'} ppm</span></div>
        <div class="metric"><span class="label">Odor</span><span class="value">${fmt(n1.odor,2)} ppm</span></div>
        <div class="metric"><span class="label">HCHO</span><span class="value">${fmt(n1.hcho,2)} ppm</span></div>
        <div class="metric"><span class="label">CO</span><span class="value">${fmt(n1.co,2)} ppm</span></div>
        <div class="metric"><span class="label">Pred</span><span class="value">${n1.pred} (${n1.class||'--'}, ${n1.conf!==undefined?Math.round(n1.conf*100):'--'}%)</span></div>
        <div class="metric"><span class="label">Fresh</span><span class="value">${n1.fresh!==undefined?n1.fresh:'--'}</span></div>
      `:'<div class="nodata">Waiting for data...</div>'}
    </div>
    <div class="card">
      <h2><span class="status-dot ${n2.valid?'green':'yellow'}"></span>Node S3-B</h2>
      ${n2.valid?`
        <div class="metric"><span class="label">VOC</span><span class="value">${fmt(n2.voc,1)} ppm</span></div>
        <div class="metric"><span class="label">CO₂</span><span class="value">${n2.co2||'--'} ppm</span></div>
        <div class="metric"><span class="label">Odor</span><span class="value">${fmt(n2.odor,2)} ppm</span></div>
        <div class="metric"><span class="label">HCHO</span><span class="value">${fmt(n2.hcho,2)} ppm</span></div>
        <div class="metric"><span class="label">CO</span><span class="value">${fmt(n2.co,2)} ppm</span></div>
        <div class="metric"><span class="label">Pred</span><span class="value">${n2.pred} (${n2.class||'--'}, ${n2.conf!==undefined?Math.round(n2.conf*100):'--'}%)</span></div>
        <div class="metric"><span class="label">Fresh</span><span class="value">${n2.fresh!==undefined?n2.fresh:'--'}</span></div>
      `:'<div class="nodata">Waiting for data...</div>'}
    </div>`;
  const ai=data.ai_result||'';
  const ac=document.getElementById('aiContent');
  if(ac)ac.innerHTML=ai?'<pre>'+escapeHtml(ai)+'</pre>':'<div class="nodata">No analysis yet. Press "AI Analysis" on the P4 screen.</div>';
}
/* Camera: preload-then-swap for smooth streaming (no flicker) */
let camLoading=false, camFrames=0, camT0=0;
function loadCam(){
  if(camLoading)return;
  camLoading=true;
  const img=new Image();
  img.onload=function(){
    const el=document.getElementById('camImg');
    if(el){el.src=img.src;}
    camLoading=false;
    camFrames++;
    if(!camT0)camT0=Date.now();
    if(camFrames%10===0){
      const fps=(camFrames*1000/(Date.now()-camT0)).toFixed(1);
      const fe=document.getElementById('fps');
      if(fe)fe.textContent='('+fps+' fps)';
    }
    setTimeout(loadCam,30);
  };
  img.onerror=function(){
    camLoading=false;
    setTimeout(loadCam,500);
  };
  img.src='/camera.jpg?t='+Date.now();
}
async function refreshData(){
  try{
    const r=await fetch('/api/data');
    const d=await r.json();
    renderData(d);
  }catch(e){console.error('fetch error',e)}
}
async function loadHistory(){
  try{
    const r=await fetch('/api/history');
    const d=await r.json();
    const hc=document.getElementById('historyContent');
    const hcnt=document.getElementById('historyCount');
    if(!hc)return;
    if(!d||d.length===0){
      hc.innerHTML='<div class="nodata">No analysis history yet.</div>';
      if(hcnt)hcnt.textContent='(0)';
      return;
    }
    if(hcnt)hcnt.textContent='('+d.length+')';
    let html='';
    const reversed=[...d].reverse();
    for(const e of reversed){
      const ts=e.ts?new Date(e.ts*1000).toLocaleString():'--';
      let resultText=e.result||'(empty)';
      try{
        const parsed=JSON.parse(resultText);
        resultText=parsed.analysis||resultText;
      }catch(e2){}
      const preview=resultText.length>200?resultText.substring(0,200)+'...':resultText;
      html+='<div style="background:#0B1020;border-radius:8px;padding:12px;margin-bottom:8px;border:1px solid #26324A">';
      html+='<div style="font-size:11px;color:#91A0B8;margin-bottom:4px">'+ts+'</div>';
      html+='<div style="font-size:13px;line-height:1.5;white-space:pre-wrap;word-wrap:break-word">'+escapeHtml(preview)+'</div>';
      html+='</div>';
    }
    hc.innerHTML=html;
  }catch(e){console.error('history fetch error',e)}
}
refreshData();
loadHistory();
setInterval(refreshData,3000);
setInterval(loadHistory,15000);
loadCam();
</script>
</body>
</html>
)";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, HTML_PAGE);
    return ESP_OK;
}

static esp_err_t api_data_get_handler(httpd_req_t *req)
{
    ui_sensor_data_t n1, n2;
    ui_get_node1_data(&n1);
    ui_get_node2_data(&n2);

    cJSON *root=cJSON_CreateObject();
    cJSON *env=cJSON_AddObjectToObject(root,"environment");
    cJSON_AddNumberToObject(env,"temperature",g_temperature);
    cJSON_AddNumberToObject(env,"humidity",g_humidity);
    cJSON_AddNumberToObject(env,"pressure",g_pressure);

    cJSON *node1=cJSON_AddObjectToObject(root,"node1");
    cJSON_AddBoolToObject(node1,"valid",n1.valid);
    if(n1.valid){
        cJSON_AddNumberToObject(node1,"odor",n1.odor);
        cJSON_AddNumberToObject(node1,"hcho",n1.hcho);
        cJSON_AddNumberToObject(node1,"co",n1.co);
        cJSON_AddNumberToObject(node1,"voc",n1.voc);
        cJSON_AddNumberToObject(node1,"co2",n1.co2);
        cJSON_AddNumberToObject(node1,"pred",n1.pred);
        cJSON_AddStringToObject(node1,"class",n1.sensor_class);
        cJSON_AddNumberToObject(node1,"conf",n1.conf);
        cJSON_AddNumberToObject(node1,"fresh",n1.fresh);
    }

    cJSON *node2=cJSON_AddObjectToObject(root,"node2");
    cJSON_AddBoolToObject(node2,"valid",n2.valid);
    if(n2.valid){
        cJSON_AddNumberToObject(node2,"odor",n2.odor);
        cJSON_AddNumberToObject(node2,"hcho",n2.hcho);
        cJSON_AddNumberToObject(node2,"co",n2.co);
        cJSON_AddNumberToObject(node2,"voc",n2.voc);
        cJSON_AddNumberToObject(node2,"co2",n2.co2);
        cJSON_AddNumberToObject(node2,"pred",n2.pred);
        cJSON_AddStringToObject(node2,"class",n2.sensor_class);
        cJSON_AddNumberToObject(node2,"conf",n2.conf);
        cJSON_AddNumberToObject(node2,"fresh",n2.fresh);
    }

    const char *ai=ui_get_ai_result();
    if(ai&&strlen(ai)>0){
        cJSON_AddStringToObject(root,"ai_result",ai);
    }

    cJSON *cam=cJSON_AddObjectToObject(root,"camera");
    if(ui_is_camera_available()){
        cJSON_AddNumberToObject(cam,"width",256);
        cJSON_AddNumberToObject(cam,"height",144);
        cJSON_AddBoolToObject(cam,"available",true);
    } else {
        cJSON_AddBoolToObject(cam,"available",false);
    }

    char *json=cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(!json){
        httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"json error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");
    httpd_resp_sendstr(req,json);
    free(json);
    return ESP_OK;
}

static esp_err_t api_history_get_handler(httpd_req_t *req)
{
    char *json = NULL;
    const char *history = ui_get_ai_history_json();
    if (history) {
        json = (char *)history;  // ui_get_ai_history_json returns malloc'd string
    } else {
        json = strdup("[]");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* Camera JPEG handler - reads pre-encoded buffer, copies to stack for safe send */
esp_err_t cam_jpg_get_handler(httpd_req_t *req)
{
    uint8_t *jpeg_buf = NULL;
    uint32_t w = 0, h = 0;
    size_t jpeg_len = 0;

    if (!ui_get_camera_frame(&jpeg_buf, &w, &h, &jpeg_len) || !jpeg_buf || jpeg_len == 0) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Camera not available");
        return ESP_OK;
    }

    /* Copy JPEG data to stack buffer so HTTP send won't race with encoder */
    /* Max JPEG size for 256x144 q50 is ~15KB, well within stack */
    size_t safe_len = (jpeg_len > 32768) ? 32768 : jpeg_len;
    uint8_t *send_buf = (uint8_t *)malloc(safe_len);
    if (!send_buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    memcpy(send_buf, jpeg_buf, safe_len);
    jpeg_len = safe_len;

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, (const char *)send_buf, jpeg_len);
    free(send_buf);
    return ret;
}

esp_err_t web_dashboard_init(void)
{
    httpd_handle_t server=NULL;
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();
    config.server_port=80;
    config.lru_purge_enable=true;
    config.max_uri_handlers = 10;

    esp_err_t err=httpd_start(&server,&config);
    if(err!=ESP_OK){
        ESP_LOGE(TAG,"Failed to start HTTP server: %s",esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_root={
        .uri="/",
        .method=HTTP_GET,
        .handler=root_get_handler,
        .user_ctx=NULL
    };
    httpd_register_uri_handler(server,&uri_root);

    httpd_uri_t uri_api={
        .uri="/api/data",
        .method=HTTP_GET,
        .handler=api_data_get_handler,
        .user_ctx=NULL
    };
    httpd_register_uri_handler(server,&uri_api);

    httpd_uri_t uri_cam={
        .uri="/camera.jpg",
        .method=HTTP_GET,
        .handler=cam_jpg_get_handler,
        .user_ctx=NULL
    };
    httpd_register_uri_handler(server, &uri_cam);

    httpd_uri_t uri_history={
        .uri="/api/history",
        .method=HTTP_GET,
        .handler=api_history_get_handler,
        .user_ctx=NULL
    };
    httpd_register_uri_handler(server, &uri_history);

    ESP_LOGI(TAG,"Web dashboard started: http://192.168.110.100");
    return ESP_OK;
}
