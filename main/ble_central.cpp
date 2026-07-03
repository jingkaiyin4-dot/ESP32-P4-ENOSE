#include "ble_central.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "cJSON.h"

static const char *TAG = "BLE_CENTRAL";

static ble_sensor_data_cb_t s_user_cb = NULL;

static const ble_uuid128_t g_enose_svc_uuid =
    BLE_UUID128_INIT(0xb0, 0xcc, 0xe0, 0xeb, 0x0a, 0x7a, 0x0c, 0x4b,
                     0x8a, 0x1a, 0x6f, 0xfe, 0xd7, 0xa7, 0xca, 0xaa);
static const ble_uuid128_t g_enose_data_chr_uuid =
    BLE_UUID128_INIT(0xb1, 0xcc, 0xe0, 0xeb, 0x0a, 0x7a, 0x0c, 0x4b,
                     0x8a, 0x1a, 0x6f, 0xfe, 0xd7, 0xa7, 0xca, 0xaa);

#define PEER_MAX 4
struct peer_info {
    uint16_t conn_handle;
    ble_addr_t addr;
    uint16_t data_val_handle;
    uint16_t write_val_handle;
    bool subscribed;
    bool connected;
};
static struct peer_info s_peers[PEER_MAX];
static int s_peer_count = 0;
static bool s_scanning = false;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static uint32_t s_last_gatt_event = 0;

static int blecentral_gap_event(struct ble_gap_event *event, void *arg);

static struct peer_info *peer_find_by_conn(uint16_t conn_handle) {
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].conn_handle == conn_handle) return &s_peers[i];
    }
    return NULL;
}

static struct peer_info *peer_find_by_addr(const ble_addr_t *addr) {
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(&s_peers[i].addr, addr, sizeof(ble_addr_t)) == 0) return &s_peers[i];
    }
    return NULL;
}

static struct peer_info *peer_add(const ble_addr_t *addr) {
    if (s_peer_count >= PEER_MAX) return NULL;
    struct peer_info *p = &s_peers[s_peer_count++];
    memset(p, 0, sizeof(*p));
    memcpy(&p->addr, addr, sizeof(ble_addr_t));
    return p;
}

static void start_scanning(void) {
    if (s_scanning) return;
    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.itvl = 160;
    disc_params.window = 80;
    disc_params.filter_policy = BLE_HCI_CONN_FILT_NO_WL;
    disc_params.limited = 0;
    disc_params.passive = 0;
    disc_params.filter_duplicates = 1;

    int rc = ble_gap_disc(s_own_addr_type, 0, &disc_params, blecentral_gap_event, NULL);
    if (rc == 0) {
        s_scanning = true;
        ESP_LOGI(TAG, "Started scanning (own_addr=%d)", s_own_addr_type);
    } else {
        ESP_LOGE(TAG, "Failed to start scanning; rc=%d", rc);
    }
}

static void stop_scanning(void) {
    if (!s_scanning) return;
    ble_gap_disc_cancel();
    s_scanning = false;
}

static int parse_sensor_json(const char *json, ble_sensor_data_t *out) {
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGW(TAG, "cJSON error near: %.40s", err ? err : "unknown");
        return -1;
    }

    // Helper: read field value from root or from "avg" sub-object
    // S3 sends abbreviated keys: n,o,h,c,v,cls,fr,uv,ua,ur,ud
    auto get_val = [&](const char *key1, const char *key2) -> cJSON* {
        cJSON *item = cJSON_GetObjectItem(root, key1);
        if (!item && key2) {
            cJSON *avg = cJSON_GetObjectItem(root, "avg");
            if (avg) item = cJSON_GetObjectItem(avg, key2);
            if (!item && avg) item = cJSON_GetObjectItem(avg, key1);
        }
        return item;
    };

    auto get_str = [&](const char *key1, const char *key2) -> const char* {
        cJSON *item = get_val(key1, key2);
        if (item && cJSON_IsString(item)) return item->valuestring;
        return NULL;
    };

    // "node" or "n" for legacy compatibility
    const char *node_str = get_str("node", NULL);
    if (!node_str) node_str = get_str("n", NULL);
    if (node_str) {
        strncpy(out->node, node_str, sizeof(out->node) - 1);
    } else {
        // Try "n" as integer (sample count) — not the node name; fallback
        cJSON *n_item = cJSON_GetObjectItem(root, "n");
        if (n_item && cJSON_IsNumber(n_item) && cJSON_GetObjectItem(root, "node") == NULL) {
            // This is the abbreviated format without "node" key
            strncpy(out->node, "S3_Receiver", sizeof(out->node) - 1);
        }
    }

    cJSON *item;

    item = get_val("o", "odor");
    if (item) out->odor = (float)item->valuedouble;

    item = get_val("h", "hcho");
    if (item) out->hcho = (float)item->valuedouble;

    item = get_val("c", "co");
    if (item) out->co = (float)item->valuedouble;

    item = get_val("v", "voc");
    if (item) out->voc = (float)item->valuedouble;

    item = get_val("co2", NULL);
    if (item) out->co2 = item->valueint;

    item = cJSON_GetObjectItem(root, "pred");
    if (item) out->pred = item->valueint;

    const char *cls_str = get_str("cls", "class");
    if (cls_str) {
        strncpy(out->sensor_class, cls_str, sizeof(out->sensor_class) - 1);
    }

    item = cJSON_GetObjectItem(root, "conf");
    if (item) out->conf = (float)item->valuedouble;

    item = get_val("fr", "fresh");
    if (item) out->fresh = item->valueint;

    // "uv" can be bool (legacy) or int 0/1 (abbreviated)
    item = get_val("uv", NULL);
    if (item) out->uv = cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint != 0);

    // "ua" = uv_auto, can be bool or int 0/1
    item = get_val("ua", "uv_auto");
    if (item) out->uv_auto = cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint != 0);

    item = get_val("ur", "uv_remain");
    if (item) out->uv_remain = item->valueint;

    item = get_val("ud", "uv_dur");
    if (item) out->uv_dur = item->valueint;

    cJSON_Delete(root);
    return (out->node[0] != '\0') ? 0 : -1;
}

static void on_sensor_data_received(struct peer_info *peer, const char *data, int len) {
    ble_sensor_data_t sensor_data;

    ESP_LOGI(TAG, "Raw (%d): %s", len, data);

    if (len < 2 || data[len - 1] != '}') {
        ESP_LOGW(TAG, "Truncated JSON (%d bytes, ends with '0x%02x'), skipped", len, (unsigned char)data[len - 1]);
        return;
    }

    if (parse_sensor_json(data, &sensor_data) != 0) {
        ESP_LOGW(TAG, "Parse failed: %s", data);
        return;
    }

    ESP_LOGI(TAG, "BLE RX node=%s voc=%.1f co2=%d odor=%.2f hcho=%.2f co=%.2f pred=%d class=%s conf=%.2f",
             sensor_data.node, (double)sensor_data.voc, sensor_data.co2,
             (double)sensor_data.odor, (double)sensor_data.hcho, (double)sensor_data.co,
             sensor_data.pred, sensor_data.sensor_class, (double)sensor_data.conf);

    if (s_user_cb) {
        s_user_cb(&sensor_data);
    }
}

static int on_disc_chrs_entry(uint16_t conn_handle, const struct ble_gatt_error *error,
                               const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Characteristic discovery complete");
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "Characteristic discovery error; status=%d", error->status);
        return 0;
    }
    struct peer_info *peer = (struct peer_info *)arg;
    if (!peer) return 0;

    if (chr->properties & BLE_GATT_CHR_PROP_NOTIFY) {
        peer->data_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found NOTIFY characteristic; val_handle=%d", chr->val_handle);

        uint8_t cccd_val[2] = {0x01, 0x00};
        int rc = ble_gattc_write_flat(peer->conn_handle, chr->val_handle + 1,
                                       cccd_val, sizeof(cccd_val), NULL, NULL);
        if (rc == 0) {
            peer->subscribed = true;
            ESP_LOGI(TAG, "Subscribed to notifications");
        } else {
            ESP_LOGE(TAG, "Failed to subscribe; rc=%d", rc);
        }
    }

    if (chr->properties & BLE_GATT_CHR_PROP_WRITE) {
        peer->write_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found WRITE characteristic; val_handle=%d", chr->val_handle);
    }

    return 0;
}

static int on_disc_all_svcs_entry(uint16_t conn_handle, const struct ble_gatt_error *error,
                                   const struct ble_gatt_svc *svc, void *arg) {
    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "=== All services discovered ===");
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "Service discovery error; status=%d", error->status);
        return 0;
    }
    struct peer_info *peer = (struct peer_info *)arg;
    if (!peer) return 0;

    if (svc->uuid.u.type == BLE_UUID_TYPE_128) {
        ESP_LOGI(TAG, "Found 128-bit service start=0x%x end=0x%x, discovering characteristics",
                 svc->start_handle, svc->end_handle);
        int rc = ble_gattc_disc_all_chrs(peer->conn_handle, svc->start_handle,
                                          svc->end_handle,
                                          on_disc_chrs_entry, peer);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to discover characteristics; rc=%d", rc);
        }
    }
    return 0;
}

static void discover_services(struct peer_info *peer) {
    int rc = ble_gattc_disc_all_svcs(peer->conn_handle,
                                      on_disc_all_svcs_entry, peer);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to discover services; rc=%d", rc);
    }
}

static int blecentral_gap_event(struct ble_gap_event *event, void *arg) {
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
        if (rc != 0) return 0;

        char addr_str[18];
        snprintf(addr_str, sizeof(addr_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 event->disc.addr.val[5], event->disc.addr.val[4],
                 event->disc.addr.val[3], event->disc.addr.val[2],
                 event->disc.addr.val[1], event->disc.addr.val[0]);

        char name_buf[64] = "(none)";
        if (fields.name) {
            int name_len = fields.name_len < 63 ? fields.name_len : 63;
            memcpy(name_buf, fields.name, name_len);
            name_buf[name_len] = '\0';
        }

        ESP_LOGI(TAG, "DISC: %s rssi=%d name=\"%s\" uuid128=%d uuid16=%d",
                  addr_str, event->disc.rssi, name_buf,
                  fields.num_uuids128, fields.num_uuids16);

        bool match = false;

        if (fields.num_uuids128 > 0) {
            for (int i = 0; i < fields.num_uuids128; i++) {
                if (ble_uuid_cmp(&fields.uuids128[i].u, &g_enose_svc_uuid.u) == 0) {
                    match = true;
                    ESP_LOGI(TAG, "  -> matched by 128-bit UUID");
                    break;
                }
            }
        }
        if (!match && fields.num_uuids16 > 0) {
            for (int i = 0; i < fields.num_uuids16; i++) {
                if (ble_uuid_cmp(&fields.uuids16[i].u, &g_enose_svc_uuid.u) == 0) {
                    match = true;
                    ESP_LOGI(TAG, "  -> matched by 16-bit UUID");
                    break;
                }
            }
        }
        if (!match && fields.name) {
            if (strstr(name_buf, "S3") != NULL || strstr(name_buf, "E-Nose") != NULL) {
                match = true;
                ESP_LOGI(TAG, "  -> matched by name");
            }
        }

        if (!match) return 0;

        struct peer_info *peer = peer_find_by_addr(&event->disc.addr);
        if (!peer) {
            peer = peer_add(&event->disc.addr);
        }
        if (peer && !peer->connected) {
            stop_scanning();
            ESP_LOGI(TAG, "Connecting to %s ...", addr_str);
            rc = ble_gap_connect(s_own_addr_type, &event->disc.addr,
                                 30000, NULL, blecentral_gap_event, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Connect failed; rc=%d", rc);
                start_scanning();
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Connected OK; conn_handle=%d (waiting link_estab)", event->connect.conn_handle);
        } else {
            ESP_LOGE(TAG, "Connect FAILED; status=%d", event->connect.status);
            start_scanning();
        }
        return 0;

    case BLE_GAP_EVENT_LINK_ESTAB:
        if (event->link_estab.status == 0) {
            ESP_LOGI(TAG, "Link established; conn_handle=%d", event->link_estab.conn_handle);
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->link_estab.conn_handle, &desc) == 0) {
                struct peer_info *peer = peer_find_by_addr(&desc.peer_ota_addr);
                if (!peer) {
                    peer = peer_add(&desc.peer_ota_addr);
                }
                if (peer) {
                    peer->conn_handle = event->link_estab.conn_handle;
                    peer->connected = true;
                    ble_gattc_exchange_mtu(event->link_estab.conn_handle, NULL, NULL);
                }
            }
        } else {
            ESP_LOGE(TAG, "Link establishment FAILED; status=%d", event->link_estab.status);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated; conn_handle=%d mtu=%d", event->mtu.conn_handle, event->mtu.value);
        {
            struct peer_info *peer = peer_find_by_conn(event->mtu.conn_handle);
            if (peer) {
                discover_services(peer);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Disconnected; reason=%d conn_handle=%d", event->disconnect.reason,
                 event->disconnect.conn.conn_handle);
        {
            struct peer_info *peer = peer_find_by_conn(event->disconnect.conn.conn_handle);
            if (peer) {
                peer->conn_handle = BLE_HS_CONN_HANDLE_NONE;
                peer->connected = false;
                peer->subscribed = false;
                peer->data_val_handle = 0;
                peer->write_val_handle = 0;
            }
        }
        start_scanning();
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        struct peer_info *peer = peer_find_by_conn(event->notify_rx.conn_handle);
        if (!peer) return 0;
        int len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > 0) {
            char *buf = (char *)malloc(len + 1);
            if (buf) {
                ble_hs_mbuf_to_flat(event->notify_rx.om, buf, len, NULL);
                buf[len] = '\0';
                on_sensor_data_received(peer, buf, len);
                free(buf);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        if (s_peer_count < PEER_MAX) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            start_scanning();
        }
        return 0;

    default:
        return 0;
    }
}

static void on_ble_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure addr; rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer addr type; rc=%d", rc);
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(s_own_addr_type, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "P4 BLE addr type=%d:", s_own_addr_type);
        ESP_LOG_BUFFER_HEX(TAG, addr_val, 6);
    }

    start_scanning();
}

static void on_ble_reset(int reason) {
    ESP_LOGE(TAG, "BLE reset; reason=%d", reason);
}

static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_central_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return;
    }

    ble_hs_cfg.reset_cb = on_ble_reset;
    ble_hs_cfg.sync_cb = on_ble_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_svc_gap_device_name_set("P4-E-Nose");
    assert(rc == 0);

    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE Central initialized");
}

void ble_central_register_cb(ble_sensor_data_cb_t cb) {
    s_user_cb = cb;
}

int ble_central_send_data(const char *data) {
    if (!data) return -1;

    int len = strlen(data);
    if (len == 0) return -1;

    int sent_count = 0;
    for (int i = 0; i < s_peer_count; i++) {
        if (!s_peers[i].connected || s_peers[i].write_val_handle == 0) {
            continue;
        }

        int rc = ble_gattc_write_flat(s_peers[i].conn_handle,
                                       s_peers[i].write_val_handle,
                                       data, len, NULL, NULL);
        if (rc == 0) {
            sent_count++;
            ESP_LOGI(TAG, "Sent %d bytes to peer %d (conn_handle=%d)", len, i, s_peers[i].conn_handle);
        } else {
            ESP_LOGW(TAG, "Write to peer %d failed; rc=%d (peer may have disconnected during analysis)", i, rc);
        }
    }

    if (sent_count == 0) {
        ESP_LOGW(TAG, "No connected peers to send data to");
    }

    return sent_count;
}

void ble_central_stop_scan(void) {
    stop_scanning();
}

void ble_central_start_scan(void) {
    start_scanning();
}
