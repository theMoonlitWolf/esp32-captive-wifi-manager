#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "Wifi.h"


// --- Define variables, classes ---
const char *TAG = "main";  ///< Log tag for this module

// --- Define functions ---
esp_err_t root_handler(httpd_req_t *req) {
    const char* resp_str = "Hello, this is the root!";
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // Upgrade to WebSocket
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = NULL;

    // First, get frame metadata
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    ws_pkt.payload = calloc(1, ws_pkt.len + 1);
    if (!ws_pkt.payload) return ESP_ERR_NO_MEM;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(ws_pkt.payload);
        return ret;
    }
    ws_pkt.payload[ws_pkt.len] = 0;

    ESP_LOGI(TAG, "Received WebSocket message: %s", (char*)ws_pkt.payload);

    free(ws_pkt.payload);
    return ESP_OK;
}


void app_main(void)
{
    // Set log level
    esp_log_level_set("*", ESP_LOG_DEBUG);
    ESP_LOGI(TAG, "START %s from %s", __FILE__, __DATE__);
    ESP_LOGI(TAG, "Setting up...");

    // Configure GPIO 3V3 bus output
    gpio_config_t v_bus_config = {
        .pin_bit_mask = (1ULL << 47),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&v_bus_config));
    ESP_ERROR_CHECK(gpio_set_level(47, 1));

   
    wifi_init();

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler
    };
    wifi_register_http_handler(&root_uri);

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    wifi_register_http_handler(&ws_uri);
}

