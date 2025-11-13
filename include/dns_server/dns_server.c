/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/**
 * @file dns_server.c
 * @brief DNS server implementation for captive portal support
 * 
 * Implements a simple DNS server that listens on UDP port 53 and responds
 * to DNS A (IPv4) queries according to configured rules. Used primarily
 * for captive portal implementations to redirect all DNS queries to the
 * device's IP address.
 */

#include <sys/param.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "dns_server.h"

/** @brief DNS server listens on UDP port 53 (standard DNS port) */
#define DNS_PORT (53)

/** @brief Maximum length of DNS packet we can handle */
#define DNS_MAX_LEN (256)

/** @brief Mask to extract OPCODE field from DNS flags */
#define OPCODE_MASK (0x7800)

/** @brief Query/Response flag (bit 15 of flags): 0=query, 1=response */
#define QR_FLAG (1 << 7)

/** @brief DNS query type for IPv4 address (A record) */
#define QD_TYPE_A (0x0001)

/** @brief Time-To-Live for DNS answers in seconds */
#define ANS_TTL_SEC (300)

/** @brief Logging tag for DNS server messages */
static const char *TAG = "dns_redirect_server";

/**
 * @brief DNS header structure.
 * 
 * Packed structure representing the DNS packet header (12 bytes).
 */
typedef struct __attribute__((__packed__))
{
    uint16_t id;        ///< Transaction ID
    uint16_t flags;     ///< Flags (QR, OPCODE, AA, TC, RD, RA, Z, RCODE)
    uint16_t qd_count;  ///< Number of questions
    uint16_t an_count;  ///< Number of answers
    uint16_t ns_count;  ///< Number of authority records
    uint16_t ar_count;  ///< Number of additional records
} dns_header_t;

/**
 * @brief DNS question structure.
 * 
 * Follows the variable-length name field in a DNS question.
 */
typedef struct {
    uint16_t type;   ///< Query type (A, AAAA, MX, etc.)
    uint16_t class;  ///< Query class (usually IN for Internet)
} dns_question_t;

/**
 * @brief DNS answer structure.
 * 
 * Packed structure for DNS answer records in the response.
 */
typedef struct __attribute__((__packed__))
{
    uint16_t ptr_offset;  ///< Pointer to name (compression)
    uint16_t type;        ///< Answer type (matches question type)
    uint16_t class;       ///< Answer class (matches question class)
    uint32_t ttl;         ///< Time-To-Live in seconds
    uint16_t addr_len;    ///< Length of address data
    uint32_t ip_addr;     ///< IPv4 address (for A records)
} dns_answer_t;

/**
 * @brief DNS server internal handle structure.
 * 
 * Contains server state and configuration entries.
 */
struct dns_server_handle {
    bool started;          ///< Server running flag
    TaskHandle_t task;     ///< FreeRTOS task handle
    int num_of_entries;    ///< Number of DNS rules
    dns_entry_pair_t entry[]; ///< Flexible array of DNS entries
};

/**
 * @brief Parse DNS name from wire format to dot-separated string.
 * 
 * DNS names are stored in label format: length-byte followed by label chars,
 * repeated until a zero-length label. This function converts to "www.example.com" format.
 * 
 * @param raw_name Pointer to raw DNS name in wire format
 * @param parsed_name Buffer to store parsed dot-separated name
 * @param parsed_name_max_len Maximum size of parsed_name buffer
 * @return Pointer to first byte after the name, or NULL on error
 */
static char *parse_dns_name(char *raw_name, char *parsed_name, size_t parsed_name_max_len)
{
    char *label = raw_name;
    char *name_itr = parsed_name;
    int name_len = 0;

    // Iterate through labels until we hit the zero-length terminator
    do {
        int sub_name_len = *label;
        // (len + 1) since we are adding a '.'
        name_len += (sub_name_len + 1);
        if (name_len > parsed_name_max_len) {
            return NULL;
        }

        // Copy the sub-label that follows the length byte
        memcpy(name_itr, label + 1, sub_name_len);
        name_itr[sub_name_len] = '.';
        name_itr += (sub_name_len + 1);
        label += sub_name_len + 1;
    } while (*label != 0);

    // Terminate the final string, replacing the last '.'
    parsed_name[name_len - 1] = '\0';
    // Return pointer to first char after the name
    return label + 1;
}

/**
 * @brief Parse a DNS request and prepare a DNS response.
 * 
 * Examines the incoming DNS query packet, validates it, and creates a response
 * packet with answers based on the configured DNS rules.
 * 
 * @param req DNS request packet buffer
 * @param req_len Length of request packet
 * @param dns_reply Buffer to store DNS response packet
 * @param dns_reply_max_len Maximum size of response buffer
 * @param h DNS server handle containing configuration rules
 * @return Length of response packet on success, 0 if request should be ignored, -1 on error
 */
static int parse_dns_request(char *req, size_t req_len, char *dns_reply, size_t dns_reply_max_len, dns_server_handle_t h)
{
    if (req_len > dns_reply_max_len) {
        return -1;
    }

    // Prepare the reply by copying the request
    memset(dns_reply, 0, dns_reply_max_len);
    memcpy(dns_reply, req, req_len);

    // Parse header (network byte order conversion needed)
    dns_header_t *header = (dns_header_t *)dns_reply;
    ESP_LOGD(TAG, "DNS query with header id: 0x%X, flags: 0x%X, qd_count: %d",
             ntohs(header->id), ntohs(header->flags), ntohs(header->qd_count));

    // Not a standard query - ignore
    if ((header->flags & OPCODE_MASK) != 0) {
        return 0;
    }

    // Set Query/Response flag to indicate this is a response
    header->flags |= QR_FLAG;

    // We'll answer the same number of questions
    uint16_t qd_count = ntohs(header->qd_count);
    header->an_count = htons(qd_count);

    // Calculate reply buffer size needed
    int reply_len = qd_count * sizeof(dns_answer_t) + req_len;
    if (reply_len > dns_reply_max_len) {
        return -1;
    }

    // Pointers for building answers
    char *cur_ans_ptr = dns_reply + req_len;
    char *cur_qd_ptr = dns_reply + sizeof(dns_header_t);
    char name[128];

    // Process each question and generate an answer if rule matches
    for (int qd_i = 0; qd_i < qd_count; qd_i++) {
        char *name_end_ptr = parse_dns_name(cur_qd_ptr, name, sizeof(name));
        if (name_end_ptr == NULL) {
            ESP_LOGE(TAG, "Failed to parse DNS question: %s", cur_qd_ptr);
            return -1;
        }

        dns_question_t *question = (dns_question_t *)(name_end_ptr);
        uint16_t qd_type = ntohs(question->type);
        uint16_t qd_class = ntohs(question->class);
        ESP_LOGD(TAG, "Received type: %d | Class: %d | Question for: %s", qd_type, qd_class, name);

        // Only handle A (IPv4 address) queries
        if (qd_type == QD_TYPE_A) {
            esp_ip4_addr_t ip = { .addr = IPADDR_ANY };
            // Check configured rules to find a match
            for (int i = 0; i < h->num_of_entries; ++i) {
                // Check if name matches exactly or if we should answer all queries ("*")
                if (strcmp(h->entry[i].name, "*") == 0 || strcmp(h->entry[i].name, name) == 0) {
                    if (h->entry[i].if_key) {
                        // Use network interface's current IP
                        esp_netif_ip_info_t ip_info;
                        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey(h->entry[i].if_key), &ip_info);
                        ip.addr = ip_info.ip.addr;
                        break;
                    } else if (h->entry->ip.addr != IPADDR_ANY) {
                        // Use static IP from configuration
                        ip.addr = h->entry[i].ip.addr;
                        break;
                    }
                }
            }
            if (ip.addr == IPADDR_ANY) {
                // No rule applies, skip this question
                continue;
            }
            // Build DNS answer record
            dns_answer_t *answer = (dns_answer_t *)cur_ans_ptr;
            answer->ptr_offset = htons(0xC000 | (cur_qd_ptr - dns_reply));
            answer->type = htons(qd_type);
            answer->class = htons(qd_class);
            answer->ttl = htonl(ANS_TTL_SEC);

            ESP_LOGD(TAG, "Answer with PTR offset: 0x%" PRIX16 " and IP 0x%" PRIX32, ntohs(answer->ptr_offset), ip.addr);

            answer->addr_len = htons(sizeof(ip.addr));
            answer->ip_addr = ip.addr;
        }
    }
    return reply_len;
}

/**
 * @brief Main DNS server task.
 * 
 * Creates a UDP socket, binds to DNS port 53, and enters a loop receiving
 * and responding to DNS queries based on configured rules.
 * 
 * @param pvParameters Pointer to dns_server_handle_t structure
 */
void dns_server_task(void *pvParameters)
{
    char rx_buffer[128];
    char addr_str[128];
    int addr_family;
    int ip_protocol;
    dns_server_handle_t handle = pvParameters;

    while (handle->started) {

        // Configure socket address for IPv4, any interface, DNS port
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(DNS_PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

        // Create UDP socket
        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created");

        // Bind socket to DNS port
        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        }
        ESP_LOGI(TAG, "Socket bound, port %d", DNS_PORT);

        // Main DNS query processing loop
        while (handle->started) {
            ESP_LOGI(TAG, "Waiting for data");
            struct sockaddr_in6 source_addr; // Large enough for both IPv4 or IPv6
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

            // Error occurred during receiving
            if (len < 0) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                close(sock);
                break;
            }
            // Data received - process DNS query
            else {
                // Get the sender's IP address as string for logging
                if (source_addr.sin6_family == PF_INET) {
                    inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
                } else if (source_addr.sin6_family == PF_INET6) {
                    inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
                }

                // Null-terminate whatever we received
                rx_buffer[len] = 0;

                // Parse query and create response
                char reply[DNS_MAX_LEN];
                int reply_len = parse_dns_request(rx_buffer, len, reply, DNS_MAX_LEN, handle);

                ESP_LOGI(TAG, "Received %d bytes from %s | DNS reply with len: %d", len, addr_str, reply_len);
                if (reply_len <= 0) {
                    ESP_LOGE(TAG, "Failed to prepare a DNS reply");
                } else {
                    // Send DNS response back to client
                    int err = sendto(sock, reply, reply_len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
                    if (err < 0) {
                        ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                        // Don't break on temporary resource exhaustion - socket is still valid
                        if (errno != ENOMEM && errno != ENOBUFS) {
                            break;
                        }
                        // Brief delay to allow memory/buffers to be freed
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                }
            }
        }

        // Clean up socket on error or server stop
        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket");
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}

/**
 * @brief Start the DNS server with the given configuration.
 * 
 * Allocates memory for the server handle and configuration, then creates
 * a FreeRTOS task to handle DNS queries.
 * 
 * @param config Pointer to DNS server configuration
 * @return dns_server_handle_t Server handle on success, NULL on allocation failure
 */
dns_server_handle_t start_dns_server(dns_server_config_t *config)
{
    // Allocate handle with space for configuration entries
    dns_server_handle_t handle = calloc(1, sizeof(struct dns_server_handle) + config->num_of_entries * sizeof(dns_entry_pair_t));
    ESP_RETURN_ON_FALSE(handle, NULL, TAG, "Failed to allocate dns server handle");

    // Initialize handle and copy configuration
    handle->started = true;
    handle->num_of_entries = config->num_of_entries;
    memcpy(handle->entry, config->item, config->num_of_entries * sizeof(dns_entry_pair_t));

    // Create DNS server task
    xTaskCreate(dns_server_task, "dns_server", 4096, handle, 5, &handle->task);
    return handle;
}

/**
 * @brief Stop the DNS server and free resources.
 * 
 * Sets the stopped flag, deletes the server task, and frees the handle.
 * 
 * @param handle DNS server handle to stop and destroy
 */
void stop_dns_server(dns_server_handle_t handle)
{
    if (handle) {
        handle->started = false;
        vTaskDelete(handle->task);
        free(handle);
    }
}
