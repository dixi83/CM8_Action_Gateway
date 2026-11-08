#include "status_led.h"

#include <stdint.h>

#include "connect_logic.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_ble.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

#define TAG "STATUS_LED"
#define LED_GPIO GPIO_NUM_48
#define RMT_RESOLUTION_HZ 10000000
#define AP_BLINK_PERIOD_MS 500

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ap_mode;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_color_t;

static bool ap_mode_enabled(void) {
    bool enabled;
    portENTER_CRITICAL(&s_state_mux);
    enabled = s_ap_mode;
    portEXIT_CRITICAL(&s_state_mux);
    return enabled;
}

void status_led_set_ap_mode(bool enabled) {
    portENTER_CRITICAL(&s_state_mux);
    s_ap_mode = enabled;
    portEXIT_CRITICAL(&s_state_mux);
}

static void write_color(led_color_t color) {
    /* WS2812 pixels expect bytes in GRB order. */
    const uint8_t grb[] = {color.green, color.red, color.blue};
    const rmt_transmit_config_t tx_config = {.loop_count = 0};
    if (rmt_transmit(s_channel, s_encoder, grb, sizeof(grb), &tx_config) == ESP_OK) {
        (void)rmt_tx_wait_all_done(s_channel, pdMS_TO_TICKS(100));
    }
}

static void status_task(void *arg) {
    const led_color_t off = {0};
    const led_color_t red = {.red = 28};
    const led_color_t yellow = {.red = 24, .green = 14};
    const led_color_t green = {.green = 28};
    const led_color_t blue = {.blue = 28};
    led_color_t previous = {0xFF, 0xFF, 0xFF};
    TickType_t last_write = 0;

    for (;;) {
        led_color_t desired;
        const bool ap_mode = ap_mode_enabled();
        if (ap_mode) {
            const bool on = ((xTaskGetTickCount() /
                              pdMS_TO_TICKS(AP_BLINK_PERIOD_MS)) % 2) == 0;
            desired = on ? blue : off;
        } else if (!gateway_ble_is_cm8_connected()) {
            desired = red;
        } else if (connect_logic_get_state() == PROTOCOL_CONNECTED) {
            desired = green;
        } else {
            desired = yellow;
        }

        const TickType_t now = xTaskGetTickCount();
        const bool changed = desired.red != previous.red ||
                             desired.green != previous.green ||
                             desired.blue != previous.blue;
        if (changed || (ap_mode &&
                        now - last_write >= pdMS_TO_TICKS(AP_BLINK_PERIOD_MS))) {
            write_color(desired);
            previous = desired;
            last_write = now;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t status_led_init(void) {
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    esp_err_t ret = rmt_new_tx_channel(&channel_config, &s_channel);
    if (ret != ESP_OK) return ret;

    const rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {.duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0},
        .bit1 = {.duration0 = 6, .level0 = 1, .duration1 = 6, .level1 = 0},
        .flags.msb_first = 1,
    };
    ret = rmt_new_bytes_encoder(&encoder_config, &s_encoder);
    if (ret != ESP_OK) return ret;
    ret = rmt_enable(s_channel);
    if (ret != ESP_OK) return ret;
    if (xTaskCreate(status_task, "status_led", 3072, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WS2812 status LED initialized on GPIO%d", LED_GPIO);
    return ESP_OK;
}

#else

void status_led_set_ap_mode(bool enabled) {
    (void)enabled;
}

esp_err_t status_led_init(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
