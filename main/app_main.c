#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "connect_logic.h"
#include "esp_err.h"
#include "esp_log.h"
#include "key_logic.h"
#include "status_led.h"

static const char *TAG = "CM8_GATEWAY";

void app_main(void) {
    const esp_err_t led_ret = status_led_init();
    if (led_ret != ESP_OK) {
        ESP_LOGW(TAG, "Status LED unavailable: %s", esp_err_to_name(led_ret));
    }

    if (connect_logic_ble_init() != 0) {
        ESP_LOGE(TAG, "Bluetooth initialization failed");
        return;
    }

    key_logic_init();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
