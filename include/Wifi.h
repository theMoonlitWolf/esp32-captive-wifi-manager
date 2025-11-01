#ifndef WIFI_HANDLER_H
#define WIFI_HANDLER_H

#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"

/**
 * @brief Configuration structure for captive portal and WiFi settings.
 */
typedef struct {
    char ssid[32];              ///< SSID of the WiFi network
    uint8_t authmode;           ///< Authentication mode: 0=Open, 1=Password, 2=Enterprise
    char username[64];          ///< Username for WPA2-Enterprise (if applicable)
    char password[64];          ///< Password for the WiFi network
    bool use_static_ip;         ///< Use static IP if true, DHCP otherwise
    esp_ip4_addr_t static_ip;   ///< Static IP address (if enabled)
    bool use_mDNS;              ///< Enable mDNS if true
    char mDNS_hostname[32];     ///< mDNS hostname (e.g., "esp32")
    char service_name[64];      ///< mDNS service name (e.g., "ESP32 Web")

    char ap_ssid[32];           ///< SSID of the captive portal AP
    char ap_password[64];       ///< Password for the captive portal AP
} captive_portal_config;



esp_err_t wifi_init();
typedef esp_err_t (*wifi_http_handler_t)(httpd_req_t *r);

esp_err_t wifi_register_http_handler(httpd_uri_t *uri);
void wifi_set_led_rgb(uint32_t irgb, uint8_t brightness);

void url_decode(char *str);

#endif