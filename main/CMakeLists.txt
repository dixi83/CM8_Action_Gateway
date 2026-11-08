idf_component_register(
    SRCS
        "app_main.c"
        "gateway_ble.c"
        "cm8_input.c"
        "key_logic.c"
        "maintenance_ap.c"
        "dji_runtime_overrides.c"
        "status_led.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES dji_demo bt esp_driver_gpio esp_driver_rmt nvs_flash
                  esp_wifi esp_event esp_netif esp_http_server app_update
)

# The pinned DJI demo creates its notification worker with a 2 KB stack. Wrap
# task creation so only that worker gets the 8 KB stack required by the
# protocol parser, leaving the upstream submodule unchanged.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xTaskCreatePinnedToCore")
