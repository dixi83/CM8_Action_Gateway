#ifndef GATEWAY_BLE_H
#define GATEWAY_BLE_H

#include "cm8_input.h"
#include "esp_err.h"
#include <stdbool.h>

typedef void (*gateway_cm8_event_cb_t)(const cm8_event_t *event);

esp_err_t gateway_ble_start_cm8(void);
void gateway_ble_set_cm8_callback(gateway_cm8_event_cb_t callback);
esp_err_t gateway_ble_shutdown_for_maintenance(void);
bool gateway_ble_is_cm8_connected(void);

#endif
