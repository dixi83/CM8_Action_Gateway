#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t status_led_init(void);
void status_led_set_ap_mode(bool enabled);

#endif
