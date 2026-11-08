#include "gateway_ble.h"

#include <string.h>

#include "ble.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define TAG "GATEWAY_BLE"
#define CAMERA_APP_ID 0
#define CM8_APP_ID 1
#define CAMERA_SERVICE_UUID 0xFFF0
#define CAMERA_NOTIFY_UUID 0xFFF4
#define CAMERA_WRITE_UUID 0xFFF5
#define CAMERA_RSSI_MIN (-85)
#define APP_CAMERA_REGISTERED BIT0
#define APP_CM8_REGISTERED BIT1

ble_profile_t s_ble_profile = {
    .gattc_if = ESP_GATT_IF_NONE,
    .connection_status = {.is_connected = false},
};

static esp_gatt_if_t s_cm8_gatt_if = ESP_GATT_IF_NONE;
static uint16_t s_cm8_conn_id;
static esp_bd_addr_t s_cm8_address;
static esp_ble_addr_type_t s_cm8_addr_type = BLE_ADDR_TYPE_PUBLIC;
static bool s_cm8_connecting;
static bool s_cm8_connected;
static bool s_camera_connect_requested;
static bool s_camera_connecting;
static bool s_cm8_scan_requested;
static bool s_scan_running;
static bool s_ble_ready;
static bool s_shutting_down;
static bool s_reconnecting;
static bool s_camera_service_found;
static esp_ble_addr_type_t s_camera_addr_type = BLE_ADDR_TYPE_PUBLIC;
static ble_notify_callback_t s_camera_notify_cb;
static connect_logic_state_callback_t s_camera_disconnect_cb;
static gateway_cm8_event_cb_t s_cm8_callback;
static EventGroupHandle_t s_profile_events;
static QueueHandle_t s_cm8_events;
static esp_bt_uuid_t s_cccd_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,
};

typedef struct {
    cm8_event_t event;
} queued_cm8_notification_t;

static const esp_ble_scan_params_t s_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gattc_callback(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param);

static void start_scan_if_needed(void) {
    if (!s_ble_ready || s_shutting_down || s_scan_running ||
        (!s_cm8_scan_requested && !s_camera_connect_requested)) return;

    esp_err_t ret = esp_ble_gap_start_scanning(0);
    if (ret == ESP_OK) {
        s_scan_running = true;
        ESP_LOGI(TAG, "BLE scan started");
    } else {
        ESP_LOGE(TAG, "Could not start BLE scan: %s", esp_err_to_name(ret));
    }
}

static void start_camera_open(const esp_bd_addr_t address,
                              esp_ble_addr_type_t address_type) {
    if (!s_camera_connect_requested || s_camera_connecting ||
        s_ble_profile.connection_status.is_connected ||
        s_ble_profile.gattc_if == ESP_GATT_IF_NONE) return;

    s_camera_connecting = true;
    s_camera_addr_type = address_type;
    memcpy(s_ble_profile.remote_bda, address, sizeof(esp_bd_addr_t));
    ESP_LOGI(TAG, "Opening camera GATT link");
    const esp_err_t ret = esp_ble_gattc_open(s_ble_profile.gattc_if,
                                              s_ble_profile.remote_bda,
                                              address_type, true);
    if (ret != ESP_OK) {
        s_camera_connecting = false;
        ESP_LOGW(TAG, "Camera GATT open request failed: %s", esp_err_to_name(ret));
    }
}

static void start_cm8_open(const esp_bd_addr_t address,
                           esp_ble_addr_type_t address_type) {
    if (!s_cm8_scan_requested || s_cm8_connecting || s_cm8_connected ||
        s_cm8_gatt_if == ESP_GATT_IF_NONE) return;

    s_cm8_connecting = true;
    s_cm8_addr_type = address_type;
    memcpy(s_cm8_address, address, sizeof(esp_bd_addr_t));
    ESP_LOGI(TAG, "Opening CM8 GATT link");
    const esp_err_t ret = esp_ble_gattc_open(s_cm8_gatt_if, s_cm8_address,
                                              address_type, true);
    if (ret != ESP_OK) {
        s_cm8_connecting = false;
        ESP_LOGW(TAG, "CM8 GATT open request failed: %s", esp_err_to_name(ret));
    }
}

static bool is_camera_advertisement(esp_ble_gap_cb_param_t *param) {
    uint8_t length = 0;
    uint8_t *manufacturer = esp_ble_resolve_adv_data_by_type(
        param->scan_rst.ble_adv,
        param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len,
        ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &length);
    return manufacturer != NULL && length >= 5 &&
           manufacturer[0] == 0xAA && manufacturer[1] == 0x08 &&
           manufacturer[4] == 0xFA && param->scan_rst.rssi >= CAMERA_RSSI_MIN;
}

static bool is_cm8_advertisement(esp_ble_gap_cb_param_t *param) {
    uint8_t length = 0;
    uint8_t *name = esp_ble_resolve_adv_data_by_type(
        param->scan_rst.ble_adv,
        param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len,
        ESP_BLE_AD_TYPE_NAME_CMPL, &length);
    return name != NULL && length >= 3 && memcmp(name, "CM8", 3) == 0;
}

static void discover_camera_service(void) {
    s_camera_service_found = false;
    s_ble_profile.service_start_handle = 0;
    s_ble_profile.service_end_handle = 0;
    esp_ble_gattc_search_service(s_ble_profile.gattc_if,
                                 s_ble_profile.conn_id, NULL);
}

static void discover_camera_characteristics(void) {
    esp_gattc_char_elem_t chars[32];
    uint16_t count = sizeof(chars) / sizeof(chars[0]);
    const esp_gatt_status_t status = esp_ble_gattc_get_all_char(
        s_ble_profile.gattc_if, s_ble_profile.conn_id,
        s_ble_profile.service_start_handle,
        s_ble_profile.service_end_handle, chars, &count, 0);
    if (status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "Camera characteristic discovery failed: %d", status);
        return;
    }

    for (uint16_t i = 0; i < count; ++i) {
        if (chars[i].uuid.len != ESP_UUID_LEN_16) continue;
        if (chars[i].uuid.uuid.uuid16 == CAMERA_NOTIFY_UUID) {
            s_ble_profile.notify_char_handle = chars[i].char_handle;
            s_ble_profile.handle_discovery.notify_char_handle_found = true;
        } else if (chars[i].uuid.uuid.uuid16 == CAMERA_WRITE_UUID) {
            s_ble_profile.write_char_handle = chars[i].char_handle;
            s_ble_profile.handle_discovery.write_char_handle_found = true;
        }
    }
    ESP_LOGI(TAG, "Camera GATT handles found: notify=%u write=%u",
             s_ble_profile.notify_char_handle,
             s_ble_profile.write_char_handle);
}

static void discover_cm8_characteristics(void) {
    esp_gattc_char_elem_t chars[32];
    uint16_t count = sizeof(chars) / sizeof(chars[0]);
    const esp_gatt_status_t status = esp_ble_gattc_get_all_char(
        s_cm8_gatt_if, s_cm8_conn_id, 1, 0xFFFF, chars, &count, 0);
    if (status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "CM8 characteristic discovery failed: %d", status);
        return;
    }

    const uint16_t input_handles[] = {
        CM8_HANDLE_CONTROL, CM8_HANDLE_RECORD, CM8_HANDLE_WHEEL,
    };
    for (size_t i = 0; i < sizeof(input_handles) / sizeof(input_handles[0]); ++i) {
        bool found = false;
        for (uint16_t j = 0; j < count; ++j) {
            if (chars[j].char_handle == input_handles[i] &&
                (chars[j].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) != 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            ESP_LOGW(TAG, "CM8 notify characteristic 0x%04x not found",
                     input_handles[i]);
            continue;
        }
        const esp_err_t ret = esp_ble_gattc_register_for_notify(
            s_cm8_gatt_if, s_cm8_address, input_handles[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Could not subscribe CM8 handle 0x%04x: %s",
                     input_handles[i], esp_err_to_name(ret));
        }
    }
}

static void enable_notifications(esp_gatt_if_t gattc_if, uint16_t conn_id,
                                 uint16_t characteristic_handle) {
    uint16_t count = 1;
    esp_gattc_descr_elem_t descriptor = {0};
    const esp_gatt_status_t status = esp_ble_gattc_get_descr_by_char_handle(
        gattc_if, conn_id, characteristic_handle, s_cccd_uuid,
        &descriptor, &count);
    if (status != ESP_GATT_OK || count == 0 || descriptor.handle == 0) {
        ESP_LOGW(TAG, "No CCCD for notification characteristic 0x%04x",
                 characteristic_handle);
        return;
    }

    uint16_t enabled = 1;
    const esp_gatt_status_t write_status = esp_ble_gattc_write_char_descr(
        gattc_if, conn_id, descriptor.handle, sizeof(enabled),
        (uint8_t *)&enabled, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (write_status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Could not enable notification 0x%04x: %d",
                 characteristic_handle, write_status);
    }
}

static void cm8_dispatch_task(void *arg) {
    queued_cm8_notification_t queued;
    for (;;) {
        if (xQueueReceive(s_cm8_events, &queued, portMAX_DELAY) != pdTRUE) continue;
        gateway_cm8_event_cb_t callback = s_cm8_callback;
        if (callback != NULL) callback(&queued.event);
    }
}

static void handle_cm8_notification(const esp_ble_gattc_cb_param_t *param) {
    queued_cm8_notification_t queued = {0};
    if (!cm8_input_decode(param->notify.handle, param->notify.value,
                          param->notify.value_len, &queued.event)) return;
    queued.event.received_at_us = esp_timer_get_time();
    if (queued.event.type == CM8_EVENT_WHEEL_POSITION ||
        queued.event.type == CM8_EVENT_RECORD_RELEASE) return;
    if (xQueueSend(s_cm8_events, &queued, 0) != pdTRUE) {
        ESP_LOGW(TAG, "CM8 event queue full; dropping report");
    }
}

static void handle_camera_gatt_event(esp_gattc_cb_event_t event,
                                     esp_ble_gattc_cb_param_t *param) {
    switch (event) {
    case ESP_GATTC_OPEN_EVT:
        s_camera_connecting = false;
        s_camera_connect_requested = false;
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "Camera GATT open failed: %d", param->open.status);
            return;
        }
        s_ble_profile.conn_id = param->open.conn_id;
        memcpy(s_ble_profile.remote_bda, param->open.remote_bda,
               sizeof(esp_bd_addr_t));
        s_ble_profile.connection_status.is_connected = true;
        const esp_err_t mtu_ret = esp_ble_gattc_send_mtu_req(
            s_ble_profile.gattc_if, s_ble_profile.conn_id);
        if (mtu_ret != ESP_OK) {
            ESP_LOGW(TAG, "Camera MTU request failed: %s", esp_err_to_name(mtu_ret));
        }
        discover_camera_service();
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        ESP_LOGI(TAG, "Camera negotiated ATT MTU=%u status=%d",
                 param->cfg_mtu.mtu, param->cfg_mtu.status);
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.uuid.uuid.uuid16 == CAMERA_SERVICE_UUID) {
            s_camera_service_found = true;
            s_ble_profile.service_start_handle = param->search_res.start_handle;
            s_ble_profile.service_end_handle = param->search_res.end_handle;
        }
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK || !s_camera_service_found) {
            ESP_LOGE(TAG, "Camera service 0x%04x was not found", CAMERA_SERVICE_UUID);
            return;
        }
        discover_camera_characteristics();
        break;
    case ESP_GATTC_NOTIFY_EVT:
        if (s_camera_notify_cb != NULL) {
            s_camera_notify_cb(param->notify.value, param->notify.value_len);
        }
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        s_ble_profile.connection_status.is_connected = false;
        s_ble_profile.handle_discovery.notify_char_handle_found = false;
        s_ble_profile.handle_discovery.write_char_handle_found = false;
        s_camera_connecting = false;
        if (s_camera_disconnect_cb != NULL) s_camera_disconnect_cb();
        start_scan_if_needed();
        break;
    default:
        break;
    }
}

static void handle_cm8_gatt_event(esp_gattc_cb_event_t event,
                                  esp_ble_gattc_cb_param_t *param) {
    switch (event) {
    case ESP_GATTC_OPEN_EVT:
        s_cm8_connecting = false;
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "CM8 GATT open failed: %d", param->open.status);
            return;
        }
        s_cm8_conn_id = param->open.conn_id;
        memcpy(s_cm8_address, param->open.remote_bda, sizeof(esp_bd_addr_t));
        s_cm8_connected = true;
        esp_ble_gattc_send_mtu_req(s_cm8_gatt_if, s_cm8_conn_id);
        const esp_err_t ret = esp_ble_gattc_search_service(
            s_cm8_gatt_if, s_cm8_conn_id, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CM8 service discovery could not start: %s",
                     esp_err_to_name(ret));
        }
        ESP_LOGI(TAG, "CM8 connected");
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "CM8 service discovery failed: %d",
                     param->search_cmpl.status);
            break;
        }
        discover_cm8_characteristics();
        break;
    case ESP_GATTC_NOTIFY_EVT:
        handle_cm8_notification(param);
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        s_cm8_connected = false;
        s_cm8_connecting = false;
        ESP_LOGW(TAG, "CM8 disconnected; scanning again");
        start_scan_if_needed();
        break;
    default:
        break;
    }
}

static void gattc_callback(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) {
    if (event == ESP_GATTC_REG_EVT && param->reg.status == ESP_GATT_OK) {
        if (param->reg.app_id == CAMERA_APP_ID) {
            s_ble_profile.gattc_if = gattc_if;
            xEventGroupSetBits(s_profile_events, APP_CAMERA_REGISTERED);
        } else if (param->reg.app_id == CM8_APP_ID) {
            s_cm8_gatt_if = gattc_if;
            xEventGroupSetBits(s_profile_events, APP_CM8_REGISTERED);
        }
        return;
    }

    if (gattc_if == s_ble_profile.gattc_if &&
        (event == ESP_GATTC_OPEN_EVT || event == ESP_GATTC_SEARCH_RES_EVT ||
         event == ESP_GATTC_SEARCH_CMPL_EVT || event == ESP_GATTC_NOTIFY_EVT ||
         event == ESP_GATTC_CFG_MTU_EVT || event == ESP_GATTC_DISCONNECT_EVT)) {
        handle_camera_gatt_event(event, param);
    } else if (gattc_if == s_cm8_gatt_if &&
               (event == ESP_GATTC_OPEN_EVT || event == ESP_GATTC_NOTIFY_EVT ||
                event == ESP_GATTC_SEARCH_CMPL_EVT ||
                event == ESP_GATTC_DISCONNECT_EVT)) {
        handle_cm8_gatt_event(event, param);
    } else if (event == ESP_GATTC_REG_FOR_NOTIFY_EVT &&
               param->reg_for_notify.status == ESP_GATT_OK) {
        if (gattc_if == s_ble_profile.gattc_if) {
            enable_notifications(gattc_if, s_ble_profile.conn_id,
                                 param->reg_for_notify.handle);
        } else if (gattc_if == s_cm8_gatt_if) {
            enable_notifications(gattc_if, s_cm8_conn_id,
                                 param->reg_for_notify.handle);
        }
    }
}

static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        start_scan_if_needed();
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_scan_running = true;
        } else {
            s_scan_running = false;
            ESP_LOGW(TAG, "BLE scan failed: %d", param->scan_start_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_scan_running = false;
        start_scan_if_needed();
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;
        if (s_cm8_scan_requested && !s_cm8_connected && !s_cm8_connecting &&
            is_cm8_advertisement(param)) {
            start_cm8_open(param->scan_rst.bda, param->scan_rst.ble_addr_type);
        }
        if (s_camera_connect_requested && !s_ble_profile.connection_status.is_connected &&
            !s_camera_connecting && is_camera_advertisement(param)) {
            start_camera_open(param->scan_rst.bda, param->scan_rst.ble_addr_type);
        }
        break;
    default:
        break;
    }
}

static esp_err_t register_cm8_profile(void) {
    if (s_cm8_gatt_if != ESP_GATT_IF_NONE) return ESP_OK;
    xEventGroupClearBits(s_profile_events, APP_CM8_REGISTERED);
    esp_err_t ret = esp_ble_gattc_app_register(CM8_APP_ID);
    if (ret != ESP_OK) return ret;
    const EventBits_t bits = xEventGroupWaitBits(s_profile_events,
                                                  APP_CM8_REGISTERED, pdFALSE,
                                                  pdTRUE, pdMS_TO_TICKS(5000));
    return (bits & APP_CM8_REGISTERED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t ble_init(void) {
    if (s_ble_ready) return ESP_OK;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) return ret;

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    esp_bt_controller_config_t bt_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_config);
    if (ret != ESP_OK) return ret;
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) return ret;
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) return ret;
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) return ret;

    s_profile_events = xEventGroupCreate();
    s_cm8_events = xQueueCreate(12, sizeof(queued_cm8_notification_t));
    if (s_profile_events == NULL || s_cm8_events == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(cm8_dispatch_task, "cm8_dispatch", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_ble_gap_register_callback(gap_callback);
    if (ret != ESP_OK) return ret;
    ret = esp_ble_gattc_register_callback(gattc_callback);
    if (ret != ESP_OK) return ret;
    ret = esp_ble_gattc_app_register(CAMERA_APP_ID);
    if (ret != ESP_OK) return ret;
    const EventBits_t bits = xEventGroupWaitBits(s_profile_events,
                                                  APP_CAMERA_REGISTERED, pdFALSE,
                                                  pdTRUE, pdMS_TO_TICKS(5000));
    if ((bits & APP_CAMERA_REGISTERED) == 0) return ESP_ERR_TIMEOUT;

    ret = esp_ble_gatt_set_local_mtu(500);
    if (ret != ESP_OK) return ret;
    ret = esp_ble_gap_set_scan_params((esp_ble_scan_params_t *)&s_scan_params);
    if (ret != ESP_OK) return ret;
    s_ble_ready = true;
    return ESP_OK;
}

esp_err_t ble_start_scanning_and_connect(void) {
    if (!s_ble_ready) return ESP_ERR_INVALID_STATE;
    s_camera_connect_requested = true;
    start_scan_if_needed();
    return ESP_OK;
}

esp_err_t gateway_ble_start_cm8(void) {
    if (!s_ble_ready) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = register_cm8_profile();
    if (ret != ESP_OK) return ret;
    s_cm8_scan_requested = true;
    start_scan_if_needed();
    return ESP_OK;
}

void gateway_ble_set_cm8_callback(gateway_cm8_event_cb_t callback) {
    s_cm8_callback = callback;
}

bool gateway_ble_is_cm8_connected(void) {
    return s_cm8_connected;
}

void ble_set_reconnecting(bool flag) { s_reconnecting = flag; }
bool ble_get_reconnecting(void) { return s_reconnecting; }

esp_err_t ble_reconnect(void) {
    s_reconnecting = true;
    return ble_start_scanning_and_connect();
}

esp_err_t ble_disconnect(void) {
    if (!s_ble_profile.connection_status.is_connected) return ESP_OK;
    return esp_ble_gattc_close(s_ble_profile.gattc_if, s_ble_profile.conn_id);
}

esp_err_t ble_read(uint16_t conn_id, uint16_t handle) {
    if (!s_ble_profile.connection_status.is_connected) return ESP_ERR_INVALID_STATE;
    return esp_ble_gattc_read_char(s_ble_profile.gattc_if, conn_id, handle,
                                    ESP_GATT_AUTH_REQ_NONE);
}

esp_err_t ble_write_without_response(uint16_t conn_id, uint16_t handle,
                                     const uint8_t *data, size_t length) {
    if (!s_ble_profile.connection_status.is_connected) return ESP_ERR_INVALID_STATE;
    return esp_ble_gattc_write_char(s_ble_profile.gattc_if, conn_id, handle,
                                     length, (uint8_t *)data,
                                     ESP_GATT_WRITE_TYPE_NO_RSP,
                                     ESP_GATT_AUTH_REQ_NONE);
}

esp_err_t ble_write_with_response(uint16_t conn_id, uint16_t handle,
                                  const uint8_t *data, size_t length) {
    if (!s_ble_profile.connection_status.is_connected) return ESP_ERR_INVALID_STATE;
    return esp_ble_gattc_write_char(s_ble_profile.gattc_if, conn_id, handle,
                                     length, (uint8_t *)data,
                                     ESP_GATT_WRITE_TYPE_RSP,
                                     ESP_GATT_AUTH_REQ_NONE);
}

esp_err_t ble_register_notify(uint16_t conn_id, uint16_t char_handle) {
    if (!s_ble_profile.connection_status.is_connected) return ESP_ERR_INVALID_STATE;
    return esp_ble_gattc_register_for_notify(s_ble_profile.gattc_if,
                                              s_ble_profile.remote_bda,
                                              char_handle);
}

esp_err_t ble_unregister_notify(uint16_t conn_id, uint16_t char_handle) {
    if (!s_ble_profile.connection_status.is_connected) return ESP_ERR_INVALID_STATE;
    return esp_ble_gattc_unregister_for_notify(s_ble_profile.gattc_if,
                                                s_ble_profile.remote_bda,
                                                char_handle);
}

void ble_set_notify_callback(ble_notify_callback_t callback) {
    s_camera_notify_cb = callback;
}

void ble_set_state_callback(connect_logic_state_callback_t callback) {
    s_camera_disconnect_cb = callback;
}

esp_err_t ble_start_advertising(void) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t gateway_ble_shutdown_for_maintenance(void) {
    s_shutting_down = true;
    s_camera_connect_requested = false;
    s_cm8_scan_requested = false;
    esp_ble_gap_stop_scanning();
    if (s_ble_profile.connection_status.is_connected) {
        esp_ble_gattc_close(s_ble_profile.gattc_if, s_ble_profile.conn_id);
    }
    if (s_cm8_connected) esp_ble_gattc_close(s_cm8_gatt_if, s_cm8_conn_id);
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_err_t ret = esp_bluedroid_disable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    ret = esp_bluedroid_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    ret = esp_bt_controller_disable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    ret = esp_bt_controller_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    return esp_bt_mem_release(ESP_BT_MODE_BTDM);
}
