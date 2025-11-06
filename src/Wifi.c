/**
 * @file Wifi.c
 * @brief WiFi and captive portal management for ESP32.
 * 
 * This file implements WiFi initialization, captive portal, event handlers,
 * and web server endpoints for configuration and control.
 */

#include "Wifi.h"

#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"      // for MAC2STR macro
#include "dns_server.h"   // for captive portal DNS hijack
#include "lwip/inet.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "led_indicator.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include <dirent.h>
#include <errno.h>

#pragma region Variables & Config

static const char *NVS_NAMESPACE_WIFI = "wifi_settings";
static const char *SD_CARD_MOUNT_POINT = "/sdcard";
static const char *TAG = "Wifi";  // Log tag for this module
static const char *TAG_CAPTIVE = "Wifi-Captive_portal";  // Log tag for captive portal
static const char *TAG_SD = "Wifi-SD_Card";  // Log tag for SD card

// Handler registry
#define MAX_CUSTOM_HANDLERS 8
static httpd_uri_t custom_handlers[MAX_CUSTOM_HANDLERS];
static size_t custom_handler_count = 0;

// Event group for WiFi state management
static EventGroupHandle_t wifi_event_group;

// Event bits for various WiFi states and actions
static const int CONNECTED_BIT = BIT0;
static const int SWITCH_TO_STA_BIT = BIT1;
static const int SWITCH_TO_CAPTIVE_AP_BIT = BIT2;
static const int RECONECT_BIT = BIT3;
static const int mDNS_CHANGE_BIT = BIT4;

// HTTP server handle and configuration
httpd_handle_t server = NULL;
static int sta_fails_count = 0;
bool SD_card_present = false;

static httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
captive_portal_config captive_cfg = { 0 };
esp_netif_t *ap_netif, *sta_netif;


// HTML page binary symbols (linked at build time, defined in CMakeLists.txt)
extern const char captive_html_start[] asm("_binary_captive_html_start");
extern const char captive_html_end[] asm("_binary_captive_html_end");

enum {
    BLINK_OFF = 0,
    BLINK_LOADING,
    BLINK_LOADED,
    BLINK_WIFI_CONNECTING,
    BLINK_WIFI_CONNECTED,
    BLINK_WIFI_DISCONNECTED,
    BLINK_WIFI_AP_STARTING,
    BLINK_WIFI_AP_STARTED,
    BLINK_MAX
};

static const blink_step_t off[] = {
    {LED_BLINK_HOLD, LED_STATE_OFF, 0},
    {LED_BLINK_STOP, 0, 0}
};

static const blink_step_t loading[] = {
    {LED_BLINK_HSV, SET_HSV(0, 0, 0), 0},
    {LED_BLINK_BREATHE, LED_STATE_75_PERCENT, 500},
    {LED_BLINK_BREATHE, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0}
};

static const blink_step_t loaded[] = {
    {LED_BLINK_HSV, SET_HSV(0, 0, 0), 0},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_STOP, 0, 0}
};

static const blink_step_t wifi_connecting[] = {
    {LED_BLINK_HSV, SET_HSV(40, MAX_SATURATION, 0), 0},
    {LED_BLINK_BREATHE, LED_STATE_75_PERCENT, 500},
    {LED_BLINK_BREATHE, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0}
};

static const blink_step_t wifi_connected[] = {
    {LED_BLINK_HSV, SET_HSV(40, MAX_SATURATION, 0), 0},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_STOP, 0, 0}
};

static const blink_step_t wifi_disconnected[] = {
    {LED_BLINK_HSV, SET_HSV(0, MAX_SATURATION, 0), 0},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_STOP, 0, 0}
};

static const blink_step_t wifi_ap_starting[] = {
    {LED_BLINK_HSV, SET_HSV(210, MAX_SATURATION, 0), 0},
    {LED_BLINK_BREATHE, LED_STATE_75_PERCENT, 500},
    {LED_BLINK_BREATHE, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0}
};

static const blink_step_t wifi_ap_started[] = {
    {LED_BLINK_HSV, SET_HSV(210, MAX_SATURATION, 0), 0},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_STOP, 0, 0}
};

led_indicator_handle_t led_handle; ///< Handle for the LED indicator
const blink_step_t *led_blink_list[BLINK_MAX] = {
    [BLINK_OFF] = off,
    [BLINK_LOADING] = loading,
    [BLINK_LOADED] = loaded,
    [BLINK_WIFI_CONNECTING] = wifi_connecting,
    [BLINK_WIFI_CONNECTED] = wifi_connected,
    [BLINK_WIFI_DISCONNECTED] = wifi_disconnected,
    [BLINK_WIFI_AP_STARTING] = wifi_ap_starting,
    [BLINK_WIFI_AP_STARTED] = wifi_ap_started
};

#pragma endregion

#pragma region Functions

// WiFi initialization functions
esp_err_t mount_sd_card();
void wifi_init_captive();
void wifi_init_sta();

// NVS helper functions
void get_nvs_wifi_settings(captive_portal_config *cfg);
void set_nvs_wifi_settings(captive_portal_config *cfg);
void fill_captive_portal_config_struct(captive_portal_config *cfg);

// WiFi configuration helpers
wifi_config_t ap_wifi_config(captive_portal_config *cfg);
wifi_config_t sta_wifi_config(captive_portal_config *cfg);

// FreeRTOS task functions
void wifi_event_group_listener_task(void *pvParameter);

// HTTP handler registration helpers
void register_custom_http_handlers(void);
void register_captive_portal_handlers(void);

// HTTP request handlers
esp_err_t captive_redirect(httpd_req_t* req, httpd_err_code_t error);
esp_err_t captive_handler(httpd_req_t* req);
esp_err_t captive_post_handler(httpd_req_t* req);
esp_err_t captive_json_handler(httpd_req_t* req);
esp_err_t scan_json_handler(httpd_req_t* req);

esp_err_t not_found_handler(httpd_req_t* req, httpd_err_code_t error);

esp_err_t index_html_get_handler(httpd_req_t* req);
esp_err_t wifi_status_json_handler(httpd_req_t* req);
esp_err_t sd_file_handler(httpd_req_t* req);
esp_err_t restart_handler(httpd_req_t *req);
esp_err_t no_sd_card_handler(httpd_req_t *req);

// WiFi event handler
void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

#pragma endregion

#pragma region Initialization

/**
 * @brief Initialize WiFi and start the mode switch task.
 * 
 * Sets up event groups, network interfaces, HTTP server config,
 * and launches the WiFi mode switch FreeRTOS task.
 */
esp_err_t wifi_init() {
    esp_log_level_set(TAG, CONFIG_LOG_LEVEL_WIFI); // Set log level for WiFi component
    esp_log_level_set(TAG_CAPTIVE, CONFIG_LOG_LEVEL_WIFI); // Set log level for captive portal
    esp_log_level_set(TAG_SD, CONFIG_LOG_LEVEL_WIFI); // Set log level for SD card component

    ESP_LOGI(TAG, "Initializing WiFi...");

    // Configure LED indicator
    led_indicator_strips_config_t led_indicator_strips_cfg = {
        .led_strip_cfg = {
            .strip_gpio_num = CONFIG_PIN_WIFI_STATUS_LED,  ///< GPIO number for the LED strip
            .max_leds = 1,               ///< Maximum number of LEDs in the strip
            .led_pixel_format = LED_PIXEL_FORMAT_GRB,  ///< Pixel format
            .led_model = LED_MODEL_SK6812,  ///< LED driver model
            .flags.invert_out = 0,       ///< Invert output signal
        },
        .led_strip_driver = LED_STRIP_SPI,
        .led_strip_spi_cfg = {
            .clk_src = SPI_CLK_SRC_DEFAULT,  ///< SPI clock source
            .spi_bus = SPI3_HOST,  ///< SPI bus host
        },
    };
    led_indicator_config_t led_cfg = {
        .mode = LED_STRIPS_MODE,  ///< LED mode (e.g., LED_STRIP_MODE)
        .led_indicator_strips_config = &led_indicator_strips_cfg,
        .blink_lists = led_blink_list,
        .blink_list_num = BLINK_MAX, 
    };

    led_handle = led_indicator_create(&led_cfg);
    if (led_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create LED indicator");
    }
    
    led_indicator_start(led_handle, BLINK_LOADING); // Start LED indicator with loading animation

    if (mount_sd_card() == ESP_OK) {
        ESP_LOGI(TAG_SD, "SD card mounted successfully");
        SD_card_present = true;
    } else {
        ESP_LOGW(TAG_SD, "Falling back to basic server, running without SD card support");
        SD_card_present = false;
    }

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Register event handlers for WiFi and IP events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    // Configure HTTP server
    httpd_config.lru_purge_enable = true;
    httpd_config.max_uri_handlers = 16;
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;
    
    // Set up default HTTP server configuration
    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();
    
    // Init WiFi with RAM-only storage
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Configure WiFi to store settings in RAM only
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Initialize captive config
    fill_captive_portal_config_struct(&captive_cfg);
    strcpy(captive_cfg.ap_ssid, "ESP32-Captive-Portal");

    // Initialize NVS
    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Read NVS settings
    get_nvs_wifi_settings(&captive_cfg);
    ESP_LOGI(TAG, "STA SSID: %s, password: %s", captive_cfg.ssid, captive_cfg.password);
    if (captive_cfg.ssid[0] == 0) {
        ESP_LOGI(TAG, "No STA SSID not configured, launching captive portal AP mode...");
        xEventGroupSetBits(wifi_event_group, SWITCH_TO_CAPTIVE_AP_BIT);
    } else {
        ESP_LOGI(TAG, "STA SSID configured, switching to STA mode...");
        xEventGroupSetBits(wifi_event_group, SWITCH_TO_STA_BIT);
    }
    ESP_LOGI(TAG, "AP SSID: %s, password: %s", captive_cfg.ap_ssid, captive_cfg.ap_password);

    // Start WiFi mode switch task
    xTaskCreate(wifi_event_group_listener_task, "wifi_event_group_listener_task", 4096, NULL, 4, NULL);

    return ESP_OK;
}

esp_err_t mount_sd_card() {
    ESP_LOGI(TAG_SD, "Mounting SD card...");

    sdmmc_card_t *card;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_PIN_WIFI_SD_MOSI,
        .miso_io_num = CONFIG_PIN_WIFI_SD_MISO,
        .sclk_io_num = CONFIG_PIN_WIFI_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,  // Default transfer size
    };
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SD, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_PIN_WIFI_SD_CS;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        #ifdef CONFIG_WIFI_FORMAT_SD_ON_FAIL
        .format_if_mount_failed = true,
        #else
        .format_if_mount_failed = false,
        #endif
        .max_files = 5,                   // Maximum number of open files
        .allocation_unit_size = 16 * 1024, // Allocation unit size
    };
    ret = esp_vfs_fat_sdspi_mount(SD_CARD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SD, "Failed to mount SD card file system: %s", esp_err_to_name(ret));
        return ret;
    }

    SD_card_present = true;

    DIR *dir = opendir(SD_CARD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG_SD, "Failed to open SD card directory");
    } else {
        struct dirent *entry;
        ESP_LOGD(TAG_SD, "Files on SD card:");
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGD(TAG_SD, "  %s", entry->d_name);
        }
    }
    closedir(dir);

    return ESP_OK;
}

/**
 * @brief Initialize WiFi in captive portal AP mode.
 * 
 * Sets up the ESP32 as a WiFi access point, starts the HTTP server,
 * registers captive portal handlers, and starts a DNS server for redirection.
 */
void wifi_init_captive() {
    ESP_LOGI(TAG_CAPTIVE, "Starting AP mode for captive portal...");

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t wifi_cfg = ap_wifi_config(&captive_cfg);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    wifi_cfg = sta_wifi_config(&captive_cfg);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    int8_t max_tx_power;
    ESP_ERROR_CHECK(esp_wifi_get_max_tx_power(&max_tx_power));
    ESP_LOGI(TAG, "Max TX power is %d, setting to 44 (11dBm) for AP mode", max_tx_power);
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(44)); // 44 = 11dBm (~5-10m range)


    // Log AP IP address
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif, &ip_info);
    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
    ESP_LOGI(TAG_CAPTIVE, "Set up softAP with IP: %s", ip_addr);

    if (captive_cfg.ap_password[0] != 0) {
        ESP_LOGI(TAG_CAPTIVE, "SoftAP started: SSID:' %s' Password: '%s'", captive_cfg.ap_ssid, captive_cfg.ap_password);
    } else {
        ESP_LOGI(TAG_CAPTIVE, "SoftAP started: SSID:' %s' No password", captive_cfg.ap_ssid);
    }

    // Start HTTP server and register handlers
    ESP_LOGV(TAG_CAPTIVE, "Starting web server on port: %d", httpd_config.server_port);
    ESP_ERROR_CHECK(httpd_start(&server, &httpd_config));

    register_captive_portal_handlers();

    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_redirect));

    // Start DNS server for captive portal redirection (highjack all DNS queries)
    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
    start_dns_server(&dns_config);
}

/**
 * @brief Initialize WiFi in station (client) mode.
 * 
 * Connects to the configured WiFi network, starts the HTTP server,
 * registers handlers, and optionally starts mDNS.
 */
void wifi_init_sta() {
    ESP_LOGI(TAG, "Starting WiFi in station mode...");
    
    wifi_config_t wifi_cfg = sta_wifi_config(&captive_cfg);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Set static IP if requested
    
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_dhcpc_stop(sta_netif);
    if (captive_cfg.use_static_ip) {
        uint32_t new_ip = ntohl(captive_cfg.static_ip.addr);
        ip_info.ip.addr = captive_cfg.static_ip.addr;
        ip_info.gw.addr = htonl((new_ip & 0xFFFFFF00)|0x01);    // x.x.x.1
        ip_info.netmask.addr = htonl((255 << 24) | (255 << 16) | (255 << 8) | 0);   // 255.255.255.0
        esp_netif_set_ip_info(sta_netif, &ip_info);
    } else {
        esp_netif_set_ip_info(sta_netif, &ip_info);
        esp_netif_dhcpc_start(sta_netif);
    }
    
    // Log IP address
    esp_netif_get_ip_info(sta_netif, &ip_info);
    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
    ESP_LOGD(TAG, "Set up STA with IP: %s", ip_addr);

    ESP_LOGD(TAG, "Starting web server on port: %d", httpd_config.server_port);
    ESP_ERROR_CHECK(httpd_start(&server, &httpd_config));

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler);

    // Register captive portal HTTP handlers (on /captive_portal for STA mode)
    register_captive_portal_handlers();

    httpd_uri_t index_html_uri = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = index_html_get_handler
    };
    httpd_register_uri_handler(server, &index_html_uri);

    httpd_uri_t wifi_status_json_uri = {
        .uri = "/wifi-status.json",
        .method = HTTP_GET,
        .handler = wifi_status_json_handler,
    };
    httpd_register_uri_handler(server, &wifi_status_json_uri);

    httpd_uri_t restart_uri = {
        .uri = "/restart",
        .method = HTTP_GET,
        .handler = restart_handler
    };
    httpd_register_uri_handler(server, &restart_uri);

    if (SD_card_present) {
        // Register custom handlers
        register_custom_http_handlers();

        httpd_uri_t sd_file_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = sd_file_handler
        };
        httpd_register_uri_handler(server, &sd_file_uri);
    } else {
        httpd_uri_t no_sd_card_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = no_sd_card_handler
        };
        httpd_register_uri_handler(server, &no_sd_card_uri);
    }


    // Start mDNS if enabled
    if (captive_cfg.use_mDNS) {
        ESP_ERROR_CHECK(mdns_init());
        ESP_ERROR_CHECK(mdns_hostname_set(captive_cfg.mDNS_hostname));
        ESP_ERROR_CHECK(mdns_instance_name_set(captive_cfg.service_name));
        ESP_LOGI(TAG, "mDNS started: http://%s.local", captive_cfg.mDNS_hostname);
        ESP_LOGI(TAG, "mDNS service started: %s", captive_cfg.service_name);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
}

void register_captive_portal_handlers(void) {
    if (server == NULL) return;

    httpd_uri_t captive_uri = {
        .uri = "/captive",
        .method = HTTP_GET,
        .handler = captive_handler
    };
    httpd_register_uri_handler(server, &captive_uri);

    httpd_uri_t captive_post_uri = {
        .uri = "/captive",
        .method = HTTP_POST,
        .handler = captive_post_handler
    };
    httpd_register_uri_handler(server, &captive_post_uri);

    httpd_uri_t captive_json_uri = {
        .uri = "/captive.json",
        .method = HTTP_GET,
        .handler = captive_json_handler
    };
    httpd_register_uri_handler(server, &captive_json_uri);

    httpd_uri_t scan_json_uri = {
        .uri = "/scan.json",
        .method = HTTP_GET,
        .handler = scan_json_handler
    };
    httpd_register_uri_handler(server, &scan_json_uri);
}

esp_err_t wifi_register_http_handler(httpd_uri_t *uri) {
    if (uri == NULL || uri->uri == NULL || uri->handler == NULL) {
        ESP_LOGE(TAG, "Cannot register handler: uri or handler is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (custom_handler_count >= MAX_CUSTOM_HANDLERS) {
        ESP_LOGE(TAG, "Custom handler registry full");
        return ESP_ERR_NO_MEM;
    }
    custom_handlers[custom_handler_count] = *uri;
    custom_handler_count++;
    // Register immediately if server is running and in STA mode
    if (server) {
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_STA) {
            esp_err_t err = httpd_register_uri_handler(server, uri);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to register custom handler for %s: %s", uri->uri, esp_err_to_name(err));
            }
            return err;
        }
    }
    return ESP_OK;
}

void register_custom_http_handlers(void) {
    if (server == NULL) return;
    for (size_t i = 0; i < custom_handler_count; ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &custom_handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register custom handler for %s: %s", custom_handlers[i].uri, esp_err_to_name(err));
        }
    }
}

#pragma endregion

#pragma region NVS helpers

void get_nvs_wifi_settings(captive_portal_config *cfg) {
    ESP_LOGD(TAG, "Reading NVS WiFi settings...");
    if (cfg == NULL) {
        ESP_LOGE(TAG, "Invalid configuration pointer (== NULL)");
    }
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        size_t len = sizeof(cfg->ssid);
        nvs_get_str(nvs_handle, "ssid", cfg->ssid, &len);
        len = sizeof(cfg->password);
        nvs_get_str(nvs_handle, "password", cfg->password, &len);
        len = sizeof(cfg->ap_ssid);
        nvs_get_str(nvs_handle, "ap_ssid", cfg->ap_ssid, &len);
        len = sizeof(cfg->ap_password);
        nvs_get_str(nvs_handle, "ap_password", cfg->ap_password, &len);
        nvs_get_u8(nvs_handle, "ap_authmode", &cfg->authmode);
        nvs_get_u8(nvs_handle, "use_static_ip", (uint8_t*)&cfg->use_static_ip);
        nvs_get_u8(nvs_handle, "use_mDNS", (uint8_t*)&cfg->use_mDNS);
        nvs_get_u32(nvs_handle, "static_ip", &cfg->static_ip.addr);
        len = sizeof(cfg->mDNS_hostname);
        nvs_get_str(nvs_handle, "mDNS_hostname", cfg->mDNS_hostname, &len);
        len = sizeof(cfg->service_name);
        nvs_get_str(nvs_handle, "service_name", cfg->service_name, &len);
        nvs_close(nvs_handle);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
    }
}

void set_nvs_wifi_settings(captive_portal_config *cfg) {
    ESP_LOGD(TAG, "Writing NVS WiFi settings...");
    int8_t n = 0;
    nvs_handle_t nvs_handle;
    captive_portal_config saved_cfg = {0};
    fill_captive_portal_config_struct(&saved_cfg);
    get_nvs_wifi_settings(&saved_cfg);
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        if (strcmp(cfg->ssid, saved_cfg.ssid) != 0) {
            nvs_set_str(nvs_handle, "ssid", cfg->ssid);
            n++;
        }
        if (strcmp(cfg->password, saved_cfg.password) != 0) {
            nvs_set_str(nvs_handle, "password", cfg->password);
            n++;
        }
        if (strcmp(cfg->ap_ssid, saved_cfg.ap_ssid) != 0) {
            nvs_set_str(nvs_handle, "ap_ssid", cfg->ap_ssid);
            n++;
        }
        if (strcmp(cfg->ap_password, saved_cfg.ap_password) != 0) {
            nvs_set_str(nvs_handle, "ap_password", cfg->ap_password);
            n++;
        }
        if (cfg->authmode != saved_cfg.authmode) {
            nvs_set_u8(nvs_handle, "ap_authmode", cfg->authmode);
            n++;
        }
        if (cfg->use_static_ip != saved_cfg.use_static_ip) {
            nvs_set_u8(nvs_handle, "use_static_ip", (uint8_t)cfg->use_static_ip);
            n++;
        }
        if (cfg->use_mDNS != saved_cfg.use_mDNS) {
            nvs_set_u8(nvs_handle, "use_mDNS", (uint8_t)cfg->use_mDNS);
            n++;
        }
        if (cfg->static_ip.addr != saved_cfg.static_ip.addr) {
            nvs_set_u32(nvs_handle, "static_ip", cfg->static_ip.addr);
            n++;
        }
        if (strcmp(cfg->mDNS_hostname, saved_cfg.mDNS_hostname) != 0) {
            nvs_set_str(nvs_handle, "mDNS_hostname", cfg->mDNS_hostname);
            n++;
        }
        if (strcmp(cfg->service_name, saved_cfg.service_name) != 0) {
            nvs_set_str(nvs_handle, "service_name", cfg->service_name);
            n++;
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGD(TAG, "NVS WiFi settings written, %d changes made", n);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
    }
}

#pragma endregion

#pragma region WiFi Config Helpers

/**
 * @brief Create WiFi configuration for station (client) mode.
 * 
 * This function fills the WiFi configuration structure with the
 * provided captive portal settings.
 * 
 * @param cfg Pointer to the captive portal configuration structure.
 * 
 * @return WiFi configuration structure for station mode.
 */
wifi_config_t sta_wifi_config(captive_portal_config *cfg) {
    wifi_config_t wifi_cfg;
    esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);

    strcpy((char *)wifi_cfg.sta.ssid, cfg->ssid);
    if (cfg->authmode == 0) {
        strcpy((char *)wifi_cfg.sta.password, "");
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_LOGD(TAG, "STA config set: Authmode: 0, SSID: %s, open network (no password)", wifi_cfg.sta.ssid);
    } else {
        strcpy((char *)wifi_cfg.sta.password, cfg->password);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_LOGD(TAG, "STA config set: Authmode: 1, SSID: %s, password: %s", wifi_cfg.sta.ssid, wifi_cfg.sta.password);
    }
    
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    return wifi_cfg;
}
    
/**
 * @brief Create WiFi configuration for captive portal AP mode.
 * 
 * This function fills the WiFi configuration structure with the
 * provided captive portal settings.
 * 
 * @param cfg Pointer to the captive portal configuration structure.
 * 
 * @return WiFi configuration structure for AP mode.
 */
wifi_config_t ap_wifi_config(captive_portal_config *cfg) {
    wifi_config_t wifi_cfg;
    esp_wifi_get_config(WIFI_IF_AP, &wifi_cfg);

    strcpy((char *)wifi_cfg.ap.ssid, cfg->ap_ssid);
    strcpy((char *)wifi_cfg.ap.password, cfg->ap_password);
    wifi_cfg.ap.ssid_len = strlen(cfg->ap_ssid);
    wifi_cfg.ap.max_connection = 4;

    if (cfg->ap_password[0] == 0) {
        wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_LOGD(TAG, "AP config set: SSID: %s, password: %s, authmode: %d", wifi_cfg.ap.ssid, wifi_cfg.ap.password, wifi_cfg.ap.authmode);
    

    return wifi_cfg;
}

/**
 * @brief Fill the captive portal configuration structure with empty values.
 * 
 * This function initializes the captive portal configuration structure
 * with empty data to ensure it is ready for use.
 * 
 * @param cfg Pointer to the captive portal configuration structure to fill.
 */
void fill_captive_portal_config_struct(captive_portal_config *cfg) {
    strcpy(cfg->ssid, "");
    strcpy(cfg->password, "");
    cfg->use_static_ip = false;
    cfg->static_ip.addr = 0;
    cfg->use_mDNS = false;
    strcpy(cfg->mDNS_hostname, "");
    strcpy(cfg->service_name, "");
    strcpy(cfg->ap_ssid, "");
    strcpy(cfg->ap_password, "");
}

#pragma endregion

#pragma region FreeRTOS Tasks

/**
 * @brief FreeRTOS task to handle WiFi mode switching and related events.
 * 
 * Waits for event bits to be set and performs actions such as switching
 * between STA/AP modes, reconnecting, or updating mDNS.
 * 
 * @param pvParameter Unused.
 */
void wifi_event_group_listener_task(void *pvParameter) {
    while (1) {
        ESP_LOGD(TAG, "Waiting for event bits...");
        // Wait for any relevant event bit
        EventBits_t eventBits = xEventGroupWaitBits(
            wifi_event_group,
            SWITCH_TO_STA_BIT | SWITCH_TO_CAPTIVE_AP_BIT | RECONECT_BIT | mDNS_CHANGE_BIT,
            pdFALSE, pdFALSE, portMAX_DELAY);
        ESP_LOGD(TAG, "Recieved event bits: %s%s%s%s%s%s%s%s%s%s",
            eventBits & BIT9 ? "1" : "0",
            eventBits & BIT8 ? "1" : "0",
            eventBits & BIT7 ? "1" : "0",
            eventBits & BIT6 ? "1" : "0",
            eventBits & BIT5 ? "1" : "0",
            eventBits & BIT4 ? "1" : "0",
            eventBits & BIT3 ? "1" : "0",
            eventBits & BIT2 ? "1" : "0",
            eventBits & BIT1 ? "1" : "0",
            eventBits & BIT0 ? "1" : "0");
        vTaskDelay(100 / portTICK_PERIOD_MS);

        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
            mode = WIFI_MODE_NULL;
        }

        // Switch to STA mode
        if (eventBits & SWITCH_TO_STA_BIT) {
            ESP_LOGI(TAG, "Switching to STA mode...");
            led_indicator_stop(led_handle, BLINK_LOADING);
            led_indicator_start(led_handle, BLINK_WIFI_CONNECTING);
            if (server) {
                httpd_stop(server);
                server = NULL;
            }
            if (eventBits & CONNECTED_BIT) {
                ESP_LOGW(TAG, "Already connected to AP, no need to switch.");
                xEventGroupClearBits(wifi_event_group, SWITCH_TO_STA_BIT);
                continue;
            }
            esp_wifi_stop();
            mdns_free(); // Free mDNS if exists
            xEventGroupClearBits(wifi_event_group, SWITCH_TO_STA_BIT);
            wifi_init_sta();
        }

        // Switch to captive AP mode
        if (eventBits & SWITCH_TO_CAPTIVE_AP_BIT) {
            ESP_LOGI(TAG, "Switching to AP captive portal mode...");
            led_indicator_stop(led_handle, BLINK_LOADING);
            led_indicator_start(led_handle, BLINK_WIFI_AP_STARTING);
            if (server) {
                httpd_stop(server);
                server = NULL;
            }
            esp_wifi_disconnect();
            esp_wifi_stop();
            mdns_free(); // Free mDNS if exists
            wifi_init_captive();
            xEventGroupClearBits(wifi_event_group, SWITCH_TO_CAPTIVE_AP_BIT);
        }

        // Reconnect in STA mode
        if (eventBits & RECONECT_BIT && mode == WIFI_MODE_STA) {
            ESP_LOGD(TAG, "Reconnecting to AP...");
            esp_wifi_disconnect();
            ESP_LOGD(TAG, "Waiting for disconnect...");
            while (xEventGroupGetBits(wifi_event_group) & CONNECTED_BIT) {
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            led_indicator_start(led_handle, BLINK_WIFI_CONNECTING);

            xEventGroupClearBits(wifi_event_group, RECONECT_BIT);

            wifi_config_t wifi_cfg = sta_wifi_config(&captive_cfg);
            esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

            // Set static or dynamic IP
            esp_netif_dhcpc_stop(sta_netif);
            if (captive_cfg.use_static_ip) {
                uint32_t new_ip = ntohl(captive_cfg.static_ip.addr);
                esp_netif_ip_info_t ip_info;
                ip_info.ip.addr = captive_cfg.static_ip.addr;
                ip_info.gw.addr = htonl((new_ip & 0xFFFFFF00)|0x01);    // x.x.x.1
                ip_info.netmask.addr = htonl((255 << 24) | (255 << 16) | (255 << 8) | 0);   // 255.255.255.0
                esp_netif_set_ip_info(sta_netif, &ip_info);
            } else {
                esp_netif_ip_info_t ip_info = {0};
                esp_netif_set_ip_info(sta_netif, &ip_info);
                esp_netif_dhcpc_start(sta_netif);
            }
            esp_wifi_connect();
        }

        // Update mDNS settings
        if (eventBits & mDNS_CHANGE_BIT && mode == WIFI_MODE_STA) {
            if (captive_cfg.use_mDNS) {
                mdns_init(); // Initialize mDNS if not already done
                ESP_ERROR_CHECK(mdns_hostname_set(captive_cfg.mDNS_hostname));
                ESP_ERROR_CHECK(mdns_instance_name_set(captive_cfg.service_name));
                ESP_LOGI(TAG, "mDNS hostname updated: %s", captive_cfg.mDNS_hostname);
                ESP_LOGI(TAG, "mDNS service name updated: %s", captive_cfg.service_name);
                mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0); // Add mDNS service if not already done
            } else {
                mdns_free(); // Free mDNS if exists
                ESP_LOGI(TAG, "mDNS removed");
            }
            xEventGroupClearBits(wifi_event_group, mDNS_CHANGE_BIT);
        }
    }
} 

#pragma endregion

#pragma region Captive Portal Handlers

/**
 * @brief HTTP handler for serving the captive portal HTML page.
 */
esp_err_t captive_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const uint32_t captive_html_len = captive_html_end - captive_html_start;
    httpd_resp_send(req, (const char *)captive_html_start, captive_html_len);
    ESP_LOGD(TAG_CAPTIVE, "Captive portal page served");
    return ESP_OK;
}

/**
 * @brief HTTP error handler for redirecting to the captive portal.
 */
esp_err_t captive_redirect(httpd_req_t *req, httpd_err_code_t error) {
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/captive");
    httpd_resp_send(req, "Redirected to captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief HTTP handler for scanning available WiFi networks and returning JSON results.
 */
esp_err_t scan_json_handler(httpd_req_t *req) {
    ESP_LOGD(TAG_CAPTIVE, "Scan request received, starting WiFi scan...");
    char json[700];
    uint16_t ap_count = 0;
    wifi_scan_config_t scan_config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 0,
        .scan_time.active.max = 0
    };
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    ESP_LOGD(TAG_CAPTIVE, "Found %d access points", ap_count);
    if (ap_count > CONFIG_WIFI_SCAN_MAX_APS) {
        ap_count = CONFIG_WIFI_SCAN_MAX_APS;
        ESP_LOGD(TAG_CAPTIVE, "Limiting to %d access points", ap_count);
    }
    wifi_ap_record_t ap_records[CONFIG_WIFI_SCAN_MAX_APS];
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));

    int len = snprintf(json, sizeof(json), "{\"ap_count\": %d, \"aps\": [", ap_count);
    for (int i = 0; i < ap_count; i++) {
        char ssid[33];
        uint8_t authmode;
        snprintf(ssid, sizeof(ssid), "%s", ap_records[i].ssid);
        if (ap_records[i].authmode == WIFI_AUTH_OPEN || ap_records[i].authmode == WIFI_AUTH_OWE || ap_records[i].authmode == WIFI_AUTH_DPP) {
            authmode = 0;
        } else if (ap_records[i].authmode == WIFI_AUTH_ENTERPRISE || ap_records[i].authmode == WIFI_AUTH_WPA2_ENTERPRISE || ap_records[i].authmode == WIFI_AUTH_WPA3_ENTERPRISE || ap_records[i].authmode == WIFI_AUTH_WPA2_WPA3_ENTERPRISE || ap_records[i].authmode == WIFI_AUTH_WPA3_ENT_192 || ap_records[i].authmode == WIFI_AUTH_WPA_ENTERPRISE) {
            authmode = 2;
        } else {
            authmode = 1;
        }
        len += snprintf(json + len, sizeof(json) - len,
        "%s{\"ssid\": \"%s\", \"rssi\": %d, \"authmode\": %d}",
            (i > 0) ? "," : "",
            ssid,
            ap_records[i].rssi,
            authmode);
        if (len < 0 || len >= sizeof(json)) {
            // Buffer full, truncate
            break;
        }
    }
    if ((len + 2) < sizeof(json)) {
        json[len++] = ']';
        json[len++] = '}';
        json[len] = '\0';
    } else {
        // Buffer full, ensure valid JSON
        strncpy(json + sizeof(json) - 3, "]}", 3);
        json[sizeof(json) - 1] = '\0';
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    ESP_LOGD(TAG_CAPTIVE, "Scan results sent: %d APs; JSON: %s", ap_count, json);
    return ESP_OK;
}

/**
 * @brief HTTP handler for returning saved captive portal configuration as JSON.
 */
esp_err_t captive_json_handler(httpd_req_t *req) {
    char json[512]; // max 330 bytes
    snprintf(json, sizeof(json),
        "{\"ssid\": \"%s\", \"authmode\": %d, \"password\": \"%s\", \"use_static_ip\": %s, \"static_ip\": \"%s\", \"use_mDNS\": %s, \"mDNS_hostname\": \"%s\", \"service_name\": \"%s\"}",
        captive_cfg.ssid,
        captive_cfg.authmode,
        captive_cfg.password,
        captive_cfg.use_static_ip ? "true" : "false",
        inet_ntoa(captive_cfg.static_ip.addr),
        captive_cfg.use_mDNS ? "true" : "false",
        captive_cfg.mDNS_hostname,
        captive_cfg.service_name
    );
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    ESP_LOGD(TAG_CAPTIVE, "Captive portal JSON data sent: %s", json);
    return ESP_OK;
}

/**
 * @brief HTTP POST handler for updating captive portal configuration.
 * 
 * Parses POST data, updates config, and triggers reconnect or mDNS update as needed.
 */
esp_err_t captive_post_handler(httpd_req_t *req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    bool need_reconnect = false;
    bool need_mdns_update = false;
    bool ssid_changed = false;  // Track SSID change
    wifi_mode_t mode;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));
    ESP_LOGD(TAG_CAPTIVE, "Received POST: len=%d, mode=%d", len, mode);
    if (len > 0) {
        buf[len] = '\0';
        ESP_LOGV(TAG_CAPTIVE, "POST data: %s", buf);
        char param[32];
        if (httpd_query_key_value(buf, "ssid", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGD(TAG_CAPTIVE, "Parsed SSID: %s", param);
            if (strcmp((char*)&captive_cfg.ssid, param) != 0) {
                ssid_changed = true;  // Mark SSID as changed
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG_CAPTIVE, "SSID changed, reconnecting...");
                }
                strcpy((char*)&captive_cfg.ssid, param);
            }
        }
        if (httpd_query_key_value(buf, "authmode", param, sizeof(param)) == ESP_OK) {
            if (strcmp(param, "") == 0) {
                ESP_LOGD(TAG_CAPTIVE, "Authmode empty");
                captive_cfg.authmode = (uint8_t)-1;
            } else {
                int new_authmode = atoi(param);
                ESP_LOGD(TAG_CAPTIVE, "Parsed Authmode: %d", new_authmode);
                if (new_authmode == 2) {
                    ESP_LOGW(TAG_CAPTIVE, "Enterprise networks (authmode 2) rejected");
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_send(req, "Enterprise networks not supported", HTTPD_RESP_USE_STRLEN);
                    return ESP_OK;
                }
                if (new_authmode < 0 || new_authmode > 1) {
                    new_authmode = 1; // default to WPA/WPA2-PSK
                    ESP_LOGD(TAG_CAPTIVE, "Authmode out of range, defaulting to WPA/WPA2-Personal");
                }
                if (captive_cfg.authmode != new_authmode) {
                    if (mode == WIFI_MODE_STA) {
                        need_reconnect = true;
                        ESP_LOGD(TAG_CAPTIVE, "Authmode changed, reconnecting...");
                    }
                }
                captive_cfg.authmode = new_authmode;
            }
        }
        if (httpd_query_key_value(buf, "password", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGD(TAG_CAPTIVE, "Parsed Password: %s", param);
            
            // Safety: If SSID changed and password is empty, reject the request
            if (ssid_changed && strlen(param) == 0 && captive_cfg.authmode == 1) {
                ESP_LOGW(TAG_CAPTIVE, "SSID changed but no password provided for WPA network");
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_send(req, "Password required for new network", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
            
            if ((captive_cfg.authmode != 0 && strlen(param) != 0 && strcmp((char*)&captive_cfg.password, param) != 0) || captive_cfg.authmode == (uint8_t)-1) {
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG_CAPTIVE, "Password changed, reconnecting...");
                }
                strcpy((char*)&captive_cfg.password, param);
            } else if (captive_cfg.authmode == 0 && captive_cfg.authmode != (uint8_t)-1) {
                strcpy((char*)&captive_cfg.password, "");
            }
        }
        if (captive_cfg.authmode == (uint8_t)-1) {
            if (captive_cfg.password[0] != 0) {
                captive_cfg.authmode = 1; // WPA/WPA2-PSK
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG_CAPTIVE, "Invalid authmode corrected to WPA/WPA2-Personal, password is not empty, reconnecting...");
                }
            } else {
                captive_cfg.authmode = 0; // Open
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG_CAPTIVE, "Invalid authmode corrected to Open, password is empty, reconnecting...");
                }
            }
        }
        if (httpd_query_key_value(buf, "use_static_ip", param, sizeof(param)) == ESP_OK) {
            bool new_use_static_ip = strcmp(param, "true") == 0;
            ESP_LOGD(TAG_CAPTIVE, "Parsed Use Static IP: %s", param);
            if (captive_cfg.use_static_ip != new_use_static_ip) {
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG_CAPTIVE, "Static IP usage changed, reconnecting...");
                }
            }
            captive_cfg.use_static_ip = new_use_static_ip;
        } else {
            if (captive_cfg.use_static_ip) {
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG_CAPTIVE, "Static IP usage disabled, reconnecting...");
                }
            }
            captive_cfg.use_static_ip = false;
        }
        if (httpd_query_key_value(buf, "static_ip", param, sizeof(param)) == ESP_OK) {
            uint32_t new_ip = inet_addr(param);
            ESP_LOGD(TAG_CAPTIVE, "Parsed Static IP: %s", param);
            if (captive_cfg.static_ip.addr != new_ip && captive_cfg.use_static_ip) {
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG_CAPTIVE, "Static IP changed, reconnecting...");
                }
            }
            captive_cfg.static_ip.addr = new_ip;
        }
        if (httpd_query_key_value(buf, "use_mDNS", param, sizeof(param)) == ESP_OK) {
            bool new_use_mdns = strcmp(param, "true") == 0;
            ESP_LOGD(TAG_CAPTIVE, "Parsed Use mDNS: %s", param);
            if (captive_cfg.use_mDNS != new_use_mdns) {
                if (mode == WIFI_MODE_STA) {
                    need_mdns_update = true;
                    ESP_LOGD(TAG_CAPTIVE, "mDNS usage changed, updating...");
                }
            }
            captive_cfg.use_mDNS = new_use_mdns;
        } else {
            if (captive_cfg.use_mDNS) {
                if (mode == WIFI_MODE_STA) {
                    need_mdns_update = true;
                    ESP_LOGD(TAG_CAPTIVE, "mDNS usage disabled, updating...");
                }
            }
            captive_cfg.use_mDNS = false;
        }
        if (httpd_query_key_value(buf, "mDNS_hostname", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGD(TAG_CAPTIVE, "Parsed mDNS Hostname: %s", param);
            if ((strcmp(captive_cfg.mDNS_hostname, param) != 0)) {
                if (captive_cfg.use_mDNS) {
                    if (mode == WIFI_MODE_STA) {
                        need_mdns_update = true;
                        ESP_LOGD(TAG_CAPTIVE, "mDNS hostname changed, updating...");
                    }
                }
                strcpy(captive_cfg.mDNS_hostname, param);
            }
        }
        if (httpd_query_key_value(buf, "service_name", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGD(TAG_CAPTIVE, "Parsed Service Name: %s", param);
            if ((strcmp(captive_cfg.service_name, param) != 0)) {
                if (captive_cfg.use_mDNS) {
                    if (mode == WIFI_MODE_STA) {
                        need_mdns_update = true;
                        ESP_LOGD(TAG_CAPTIVE, "mDNS service name changed, updating...");
                    }
                }
                strcpy(captive_cfg.service_name, param);
            }
        }
    }

    // Log the updated captive portal settings
    ESP_LOGI(TAG_CAPTIVE, "Settings updated: SSID=%s, authmode=%d, static_ip=%d, mDNS=%d", 
             captive_cfg.ssid, captive_cfg.authmode, captive_cfg.use_static_ip, captive_cfg.use_mDNS);

    // Save settings to NVS
    set_nvs_wifi_settings(&captive_cfg);

    if (mode == WIFI_MODE_STA) {
        // Check if it is needed to reconnect to the AP or update and restart mDNS
        if (need_reconnect) {
            xEventGroupSetBits(wifi_event_group, RECONECT_BIT);
        }
        if (need_mdns_update) {
            xEventGroupSetBits(wifi_event_group, mDNS_CHANGE_BIT);
        }
        
        // Redirect back to captive portal, method GET
        httpd_resp_set_status(req, "302 Temporary Redirect");
        httpd_resp_set_hdr(req, "Location", "/captive");
        httpd_resp_send(req, "Redirected", HTTPD_RESP_USE_STRLEN);
        ESP_LOGV(TAG_CAPTIVE, "Redirecting to back captive portal, method GET");
        return ESP_OK;
    } else {
        ESP_LOGI(TAG_CAPTIVE, "Switching to STA mode with new config");
        xEventGroupSetBits(wifi_event_group, SWITCH_TO_STA_BIT);
        return ESP_OK;
    }
}


/**
 * @brief Decode a URL-encoded string in place.
 * 
 * @param str The URL-encoded string to decode.
 * 
 * This function modifies the input string directly to decode it.
 */
void url_decode(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            int hi = src[1], lo = src[2];
            hi = (hi >= 'A') ? (hi & ~0x20) - 'A' + 10 : hi - '0';
            lo = (lo >= 'A') ? (lo & ~0x20) - 'A' + 10 : lo - '0';
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

#pragma endregion

#pragma region STA handlers

esp_err_t no_sd_card_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h2>SD card not detected</h2>\n<p>Please insert an SD card and <a href=\"/restart\">restart</a> the device</p>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t index_html_get_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);  // Response body can be empty
    ESP_LOGD(TAG, "Redirecting to /");
    return ESP_OK;
}

/**
 * @brief HTTP handler for /restart endpoint (restarts ESP32).
 */
esp_err_t restart_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Restarting...", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}

/**
 * @brief HTTP error handler for 404.
 */
esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t error) {
    char text[256];
    size_t len = 0;
    len += snprintf(text + len, sizeof(text) - len, "404 Not Found\n\n");
    len += snprintf(text + len, sizeof(text) - len, "URI: %s\n", req->uri);
    len += snprintf(text + len, sizeof(text) - len, "Method: %s\n", (req->method == HTTP_GET) ? "GET" : "POST");
    len += snprintf(text + len, sizeof(text) - len, "Arguments:\n");
    char query[128];
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        httpd_req_get_url_query_str(req, query, query_len);
        len += snprintf(text + len, sizeof(text) - len, "%s\n", query);
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    ESP_LOGW(__FILE__, "%s", text);
    return ESP_FAIL;
}

esp_err_t wifi_status_json_handler(httpd_req_t *req) {
    char json[256];
    EventBits_t bits = xEventGroupGetBits(wifi_event_group);
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(sta_netif, &ip_info);
    bool connected = (bits & CONNECTED_BIT) != 0;
    char ip_str[IP4ADDR_STRLEN_MAX];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, IP4ADDR_STRLEN_MAX);
    snprintf(json, sizeof(json), "{\"connected\": %s, \"ip\": \"%s\"}",
             connected ? "true" : "false",
             ip_str);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    ESP_LOGD(TAG_CAPTIVE, "WiFi status JSON sent: %s", json);
    return ESP_OK;
}

esp_err_t sd_file_handler(httpd_req_t *req) {

    char filepath[530];
    snprintf(filepath, sizeof(filepath), "%s%s", SD_CARD_MOUNT_POINT, req->uri);

    struct stat st;
    if (stat(filepath, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t len = strlen(filepath);
        if (len > 0 && filepath[len - 1] == '/') {
            strcat(filepath, "index.html");
        } else {
            strcat(filepath, "/index.html");
        }
    } else if (!strchr(req->uri, '.') && stat(filepath, &st) != 0) {
        strcat(filepath, ".html");
    }

    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s (%s)", filepath, strerror(errno));
        return not_found_handler(req, HTTPD_404_NOT_FOUND);
    }

    if (strstr(filepath, ".html") || strstr(filepath, ".htm")) {
        httpd_resp_set_type(req, "text/html");
    } else if (strstr(filepath, ".css")) {
        httpd_resp_set_type(req, "text/css");
    } else if (strstr(filepath, ".js")) {
        httpd_resp_set_type(req, "application/javascript");
    } else if (strstr(filepath, ".json")) {
        httpd_resp_set_type(req, "application/json");
    } else if (strstr(filepath, ".png")) {
        httpd_resp_set_type(req, "image/png");
    } else if (strstr(filepath, ".jpg") || strstr(filepath, ".jpeg")) {
        httpd_resp_set_type(req, "image/jpeg");
    } else if (strstr(filepath, ".gif")) {
        httpd_resp_set_type(req, "image/gif");
    } else if (strstr(filepath, ".svg")) {
        httpd_resp_set_type(req, "image/svg+xml");
    } else if (strstr(filepath, ".ico")) {
        httpd_resp_set_type(req, "image/x-icon");
    } else if (strstr(filepath, ".woff")) {
        httpd_resp_set_type(req, "font/woff");
    } else if (strstr(filepath, ".woff2")) {
        httpd_resp_set_type(req, "font/woff2");
    } else if (strstr(filepath, ".ttf")) {
        httpd_resp_set_type(req, "font/ttf");
    } else if (strstr(filepath, ".otf")) {
        httpd_resp_set_type(req, "font/otf");
    } else if (strstr(filepath, ".eot")) {
        httpd_resp_set_type(req, "application/vnd.ms-fontobject");
    } else if (strstr(filepath, ".mp4")) {
        httpd_resp_set_type(req, "video/mp4");
    } else if (strstr(filepath, ".webm")) {
        httpd_resp_set_type(req, "video/webm");
    } else if (strstr(filepath, ".txt")) {
        httpd_resp_set_type(req, "text/plain");
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }

    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGD(TAG, "Serving SD file: %s", filepath);
    return ESP_OK;
}

#pragma endregion

#pragma region Wifi Event Handler

/**
 * @brief WiFi and IP event handler.
 * 
 * Handles AP/STA connect/disconnect, IP acquisition, and triggers mode switches.
 */
void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    EventBits_t bits = xEventGroupGetBits(wifi_event_group);
    wifi_mode_t mode;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "Wi-Fi AP started."); 
        led_indicator_stop(led_handle, BLINK_WIFI_AP_STARTING);
        led_indicator_start(led_handle, BLINK_WIFI_AP_STARTED);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGD(TAG, "station " MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGD(TAG, "station " MACSTR " leave, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START && mode == WIFI_MODE_STA) {
        ESP_LOGI(TAG, "Wi-Fi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
        ESP_LOGI(TAG, "Connected to AP: %s", event->ssid);
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        sta_fails_count = 0;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        led_indicator_stop(led_handle, BLINK_WIFI_CONNECTING);
        led_indicator_stop(led_handle, BLINK_WIFI_CONNECTED);
        led_indicator_start(led_handle, BLINK_WIFI_DISCONNECTED);
        if ((bits & RECONECT_BIT) == 0 && mode == WIFI_MODE_STA && (bits & SWITCH_TO_CAPTIVE_AP_BIT) == 0) {
            ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
            sta_fails_count++;
            if (sta_fails_count >= CONFIG_WIFI_MAX_RECONNECTS) {
                ESP_LOGW(TAG, "Max STA reconect fails reached, switching to AP mode...");
                esp_wifi_disconnect();
                sta_fails_count = 0;
                xEventGroupSetBits(wifi_event_group, SWITCH_TO_CAPTIVE_AP_BIT);
                return;
            } else {
                ESP_LOGD(TAG, "Reconnecting...");
                esp_wifi_connect();
                led_indicator_start(led_handle, BLINK_WIFI_CONNECTING);
            }
        } else {
            ESP_LOGD(TAG, "Wi-Fi disconnected.");
        } 
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[IP4ADDR_STRLEN_MAX];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, IP4ADDR_STRLEN_MAX);
        ESP_LOGI(TAG, "Got IP: %s", ip_str);
        sta_fails_count = 0;
        led_indicator_stop(led_handle, BLINK_WIFI_CONNECTING);
        led_indicator_start(led_handle, BLINK_WIFI_CONNECTED);
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    } else {
        ESP_LOGW(TAG, "Unhandled event: %s:%ld", event_base, event_id);
    }
}

#pragma endregion

/**
 * @brief Manually set the LED color and brightness using RGB.
 * 
 * @param irgb LED color in 0xRRGGBB format
 * @param brightness LED brightness (0-255)
 */
void wifi_set_led_rgb(uint32_t irgb, uint8_t brightness) {
    if (led_handle) {
        led_indicator_set_rgb(led_handle, irgb);
        led_indicator_set_brightness(led_handle, brightness);
    }
}
