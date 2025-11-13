/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/**
 * @file dns_server.h
 * @brief Simple DNS server for captive portal implementation
 * 
 * This component provides a lightweight DNS server that can respond to DNS queries
 * with configured IP addresses or network interface IPs. Primarily used for
 * captive portal implementations where all DNS queries need to be redirected
 * to the device's IP address.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of DNS entry rules that can be configured */
#ifndef DNS_SERVER_MAX_ITEMS
#define DNS_SERVER_MAX_ITEMS 1
#endif

/**
 * @brief Macro to simplify single-entry DNS server configuration.
 * 
 * Creates a configuration with one DNS entry that responds to a specific
 * name (or "*" for all queries) with the IP of a network interface.
 * 
 * @param queried_name DNS name to match ("*" matches all A queries)
 * @param netif_key Network interface key (e.g., "WIFI_AP_DEF" for softAP)
 */
#define DNS_SERVER_CONFIG_SINGLE(queried_name, netif_key)  {        \
        .num_of_entries = 1,                                        \
        .item = { { .name = queried_name, .if_key = netif_key } }   \
        }

/**
 * @brief DNS entry defining a name-to-IP mapping rule.
 * 
 * Each entry specifies a DNS name to match and either a network interface
 * whose IP should be used in the response, or a static IP address.
 * 
 * @note Use string literals or ensure strings remain valid during the
 *       DNS server's lifetime, as pointers are stored directly without copying.
 */
typedef struct dns_entry_pair {
    const char* name;       /**< DNS name to match exactly, or "*" to match all queries */
    const char* if_key;     /**< Network interface key to use for IP (NULL to use static IP below) */
    esp_ip4_addr_t ip;      /**< Static IPv4 address to respond with (only used if if_key is NULL) */
} dns_entry_pair_t;

/**
 * @brief DNS server configuration structure.
 * 
 * Defines the rules for responding to DNS A (IPv4) type queries. Multiple
 * rules can be configured by increasing DNS_SERVER_MAX_ITEMS before including
 * this header.
 * 
 * @note If you want to define more rules, set DNS_SERVER_MAX_ITEMS before
 *       including this header.
 * 
 * Example with 2 entries using constant IP addresses:
 * @code{.c}
 * #define DNS_SERVER_MAX_ITEMS 2
 * #include "dns_server.h"
 *
 * dns_server_config_t config = {
 *   .num_of_entries = 2,
 *   .item = { {.name = "my-esp32.com", .ip = { .addr = ESP_IP4TOADDR( 192, 168, 4, 1) } } ,
 *             {.name = "my-utils.com", .ip = { .addr = ESP_IP4TOADDR( 192, 168, 4, 100) } } } };
 * start_dns_server(&config);
 * @endcode
 */
typedef struct dns_server_config {
    int num_of_entries;                             /**< Number of DNS rules configured */
    dns_entry_pair_t item[DNS_SERVER_MAX_ITEMS];    /**< Array of DNS entry rules */
} dns_server_config_t;

/**
 * @brief Opaque DNS server handle type.
 * 
 * Handle returned by start_dns_server() and used to stop the server.
 */
typedef struct dns_server_handle *dns_server_handle_t;

/**
 * @brief Start a DNS server with the specified configuration.
 * 
 * Creates and starts a DNS server task that listens on UDP port 53.
 * The server responds to DNS A (IPv4) queries based on the configured rules.
 * 
 * Each rule can either:
 * - Respond with a network interface's current IP address (if if_key is set)
 * - Respond with a static IP address (if if_key is NULL)
 * 
 * Use "*" as the name to match all DNS queries (typical for captive portals).
 * 
 * @param config Pointer to DNS server configuration structure
 * @return dns_server_handle_t Server handle on success
 * @return NULL on failure (memory allocation error)
 */
dns_server_handle_t start_dns_server(dns_server_config_t *config);

/**
 * @brief Stop and destroy the DNS server.
 * 
 * Stops the DNS server task, closes the socket, and frees all resources.
 * 
 * @param handle DNS server handle returned by start_dns_server()
 */
void stop_dns_server(dns_server_handle_t handle);


#ifdef __cplusplus
}
#endif
