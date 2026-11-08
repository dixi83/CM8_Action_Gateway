#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"

#include <stdlib.h>

#include "command_logic.h"
#include "connect_logic.h"
#include "data.h"
#include "dji_protocol_data_structures.h"
#include "ble.h"
#include "enums_logic.h"
#include "esp_random.h"
#include "esp_log.h"
#include "gateway_ble.h"
#include "key_logic.h"
#include "maintenance_ap.h"
#include "status_logic.h"

static const char *TAG = "GATEWAY_KEY";
static portMUX_TYPE s_toggle_mux = portMUX_INITIALIZER_UNLOCKED;
static TimerHandle_t s_maintenance_timer;
static QueueHandle_t s_camera_action_queue;
static bool s_toggle_is_down;
static bool s_maintenance_requested;

#define MAINTENANCE_HOLD_MS 3000

static void maintenance_task(void *arg) {
    const esp_err_t ret = maintenance_ap_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not start firmware update AP: %s", esp_err_to_name(ret));
        portENTER_CRITICAL(&s_toggle_mux);
        s_maintenance_requested = false;
        portEXIT_CRITICAL(&s_toggle_mux);
    }
    vTaskDelete(NULL);
}

static void maintenance_timer_callback(TimerHandle_t timer) {
    bool start = false;
    portENTER_CRITICAL(&s_toggle_mux);
    if (s_toggle_is_down && !s_maintenance_requested) {
        s_maintenance_requested = true;
        start = true;
    }
    portEXIT_CRITICAL(&s_toggle_mux);

    if (start && xTaskCreate(maintenance_task, "maintenance_start", 4096,
                             NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create maintenance task");
        portENTER_CRITICAL(&s_toggle_mux);
        s_maintenance_requested = false;
        portEXIT_CRITICAL(&s_toggle_mux);
    }
}

static int connect_camera(void) {
    if (connect_logic_get_state() == PROTOCOL_CONNECTED) return 0;
    if (connect_logic_get_state() != BLE_INIT_COMPLETE) {
        ESP_LOGW(TAG, "Camera connection is already in progress");
        return -1;
    }

    if (!is_data_layer_initialized()) {
        data_init();
        data_register_status_update_callback(update_camera_state_handler);
        data_register_new_status_update_callback(update_new_camera_state_handler);
    }

    if (connect_logic_ble_connect(false) != 0) return -1;

    /* These are the connection-profile parameters validated against this
     * camera during the current hardware test. */
    const uint16_t verify_data = (uint16_t)(esp_random() % 10000);
    if (connect_logic_protocol_connect(0xFF330000,
                                      sizeof(s_ble_profile.remote_bda),
                                      (const int8_t *)s_ble_profile.remote_bda,
                                      0, 0, verify_data, 0) != 0) {
        ESP_LOGE(TAG, "Camera protocol connection failed");
        return -1;
    }

    version_query_response_frame_t *version = command_logic_get_version();
    free(version);
    if (subscript_camera_status(PUSH_MODE_PERIODIC_WITH_STATE_CHANGE,
                                PUSH_FREQ_2HZ) != 0) {
        ESP_LOGE(TAG, "Camera connected, but status subscription failed");
        return -1;
    }
    ESP_LOGI(TAG, "Osmo Action connected");
    return 0;
}

static void switch_camera_mode(void) {
    if (!camera_status_initialized) {
        ESP_LOGW(TAG, "Camera mode is not known yet");
        return;
    }

    const camera_mode_t target = current_camera_mode == CAMERA_MODE_PHOTO
                                     ? CAMERA_MODE_NORMAL
                                     : CAMERA_MODE_PHOTO;
    ESP_LOGI(TAG, "Switching camera mode %u -> %u", current_camera_mode, target);
    camera_mode_switch_response_frame_t *response =
        command_logic_switch_camera_mode(target);
    if (response == NULL) {
        ESP_LOGE(TAG, "Camera mode switch failed");
        return;
    }
    ESP_LOGI(TAG, "Camera mode switch returned %u", response->ret_code);
    free(response);
}

static void handle_camera_action(const cm8_event_t *event) {
    switch (event->type) {
    case CM8_EVENT_RECORD_SHORT:
        if (connect_logic_get_state() != PROTOCOL_CONNECTED) {
            ESP_LOGW(TAG, "Ignoring record press until camera is connected");
        } else {
            record_control_response_frame_t *response = is_camera_recording()
                ? command_logic_stop_record() : command_logic_start_record();
            if (response == NULL) {
                ESP_LOGE(TAG, "Record control command failed");
            } else {
                ESP_LOGI(TAG, "Record control returned %u", response->ret_code);
                free(response);
            }
        }
        break;
    case CM8_EVENT_RECORD_LONG:
        if (connect_logic_get_state() == PROTOCOL_CONNECTED) {
            switch_camera_mode();
        } else {
            connect_camera();
        }
        break;
    case CM8_EVENT_RECORD_DOUBLE:
        ESP_LOGI(TAG, "Ignoring CM8 double press");
        break;
    default:
        break;
    }
}

static void camera_action_task(void *arg) {
    cm8_event_t event;
    for (;;) {
        if (xQueueReceive(s_camera_action_queue, &event, portMAX_DELAY) == pdTRUE) {
            handle_camera_action(&event);
        }
    }
}

static void handle_cm8_event(const cm8_event_t *event) {
    if (event == NULL) return;

    if (event->type == CM8_EVENT_MODE_TOGGLE) {
        bool start_timer = false;
        bool stop_timer = false;
        portENTER_CRITICAL(&s_toggle_mux);
        if (event->mode_state == 0) {
            if (s_toggle_is_down) {
                s_toggle_is_down = false;
                stop_timer = true;
            }
        } else if (!s_toggle_is_down && !s_maintenance_requested) {
            s_toggle_is_down = true;
            start_timer = true;
        }
        portEXIT_CRITICAL(&s_toggle_mux);

        if (stop_timer) xTimerStop(s_maintenance_timer, 0);
        if (start_timer && s_maintenance_timer != NULL) {
            if (xTimerStart(s_maintenance_timer, 0) == pdPASS) {
                ESP_LOGI(TAG, "Hold toggle for three seconds to enter maintenance mode");
            } else {
                ESP_LOGE(TAG, "Could not start maintenance hold timer");
            }
        }
        return;
    }

    if (s_camera_action_queue == NULL ||
        xQueueSend(s_camera_action_queue, event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Camera action queue full; dropping CM8 action");
    }
}

void key_logic_init(void) {
    s_camera_action_queue = xQueueCreate(8, sizeof(cm8_event_t));
    if (s_camera_action_queue == NULL ||
        xTaskCreate(camera_action_task, "camera_actions", 6144,
                    NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start camera action worker");
        return;
    }

    s_maintenance_timer = xTimerCreate("maintenance_hold",
                                      pdMS_TO_TICKS(MAINTENANCE_HOLD_MS),
                                      pdFALSE, NULL, maintenance_timer_callback);
    if (s_maintenance_timer == NULL) {
        ESP_LOGE(TAG, "Could not create maintenance hold timer");
    }
    gateway_ble_set_cm8_callback(handle_cm8_event);
    const esp_err_t ret = gateway_ble_start_cm8();
    if (ret != ESP_OK) ESP_LOGE(TAG, "CM8 scan failed: %s", esp_err_to_name(ret));
}
