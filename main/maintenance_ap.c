#include "maintenance_ap.h"

#include <stdint.h>
#include <string.h>
#include <sys/param.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_ble.h"
#include "status_led.h"

#define TAG "MAINTENANCE_AP"
#define AP_SSID "CM8-Gateway-Setup"
#define AP_TIMEOUT_MS (10 * 60 * 1000)

static httpd_handle_t s_http_server;
static esp_netif_t *s_ap_netif;

static const char s_page[] =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>CM8 Gateway Update</title></head><body>"
    "<h2>CM8 Gateway firmware</h2><p>Select the ESP-IDF application .bin file.</p>"
    "<input id='f' type='file' accept='.bin,application/octet-stream'>"
    "<button onclick='go()'>Upload firmware</button><p id='s'></p>"
    "<progress id='p' value='0' max='100' hidden></progress>"
    "<script>function go(){const f=document.getElementById('f').files[0];"
    "if(!f){alert('Select a firmware .bin file first.');return;}"
    "const x=new XMLHttpRequest();x.open('POST','/update');"
    "x.setRequestHeader('Content-Type','application/octet-stream');"
    "const p=document.getElementById('p');p.hidden=false;"
    "x.upload.onprogress=e=>{if(e.lengthComputable)p.value=e.loaded/e.total*100};"
    "x.onload=()=>document.getElementById('s').textContent=x.responseText;"
    "x.onerror=()=>document.getElementById('s').textContent='Upload failed';"
    "document.getElementById('s').textContent='Uploading...';x.send(f);}</script></body></html>";

static esp_err_t page_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t upload_handler(httpd_req_t *req) {
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL || req->content_len == 0 || req->content_len > partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "No OTA slot or image too large.");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle;
    esp_err_t ret = esp_ota_begin(partition, req->content_len, &handle);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Could not start OTA update.");
        return ESP_FAIL;
    }

    uint8_t buffer[4096];
    size_t remaining = req->content_len;
    while (remaining > 0) {
        const size_t wanted = MIN(remaining, sizeof(buffer));
        const int received = httpd_req_recv(req, (char *)buffer, wanted);
        if (received <= 0) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Upload was interrupted.");
            return ESP_FAIL;
        }
        ret = esp_ota_write(handle, buffer, (size_t)received);
        if (ret != ESP_OK) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Could not write firmware.");
            return ESP_FAIL;
        }
        remaining -= (size_t)received;
    }

    ret = esp_ota_end(handle);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware image is invalid.");
        return ESP_FAIL;
    }
    ret = esp_ota_set_boot_partition(partition);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Could not select updated firmware.");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "Update complete. Restarting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void timeout_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(AP_TIMEOUT_MS));
    ESP_LOGW(TAG, "Maintenance window expired; restarting gateway");
    esp_restart();
}

esp_err_t maintenance_ap_start(void) {
    esp_err_t ret = gateway_ble_shutdown_for_maintenance();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Bluetooth shutdown failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) return ESP_FAIL;
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init);
    if (ret != ESP_OK) return ret;
    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) return ret;

    wifi_config_t config = {0};
    strlcpy((char *)config.ap.ssid, AP_SSID, sizeof(config.ap.ssid));
    config.ap.ssid_len = strlen(AP_SSID);
    config.ap.channel = 1;
    config.ap.max_connection = 1;
    config.ap.authmode = WIFI_AUTH_OPEN;
    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) return ret;
    ret = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (ret != ESP_OK) return ret;
    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;

    httpd_config_t server = HTTPD_DEFAULT_CONFIG();
    server.stack_size = 8192;
    ret = httpd_start(&s_http_server, &server);
    if (ret != ESP_OK) return ret;
    const httpd_uri_t page = {.uri = "/", .method = HTTP_GET,
                              .handler = page_handler};
    const httpd_uri_t update = {.uri = "/update", .method = HTTP_POST,
                                .handler = upload_handler};
    ret = httpd_register_uri_handler(s_http_server, &page);
    if (ret != ESP_OK) return ret;
    ret = httpd_register_uri_handler(s_http_server, &update);
    if (ret != ESP_OK) return ret;
    if (xTaskCreate(timeout_task, "maintenance_timeout", 2048,
                    NULL, 1, NULL) != pdPASS) return ESP_ERR_NO_MEM;

    status_led_set_ap_mode(true);
    ESP_LOGI(TAG, "AP %s ready at http://192.168.4.1/", AP_SSID);
    return ESP_OK;
}
