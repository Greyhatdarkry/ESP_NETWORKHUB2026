#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"

#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "ping/ping_sock.h"

#include "cJSON.h"

#include "diagnostics.h"

static const char *TAG = "NETWORK_DIAG";

static esp_netif_t *diagnostic_sta_netif = NULL;

typedef struct {
    SemaphoreHandle_t finished;
    uint32_t transmitted;
    uint32_t received;
    uint32_t total_rtt_ms;
    uint32_t average_rtt_ms;
    uint32_t duration_ms;
    bool completed;
} ping_result_t;

/* ---------------------------------------------------------
   PING CALLBACKS
--------------------------------------------------------- */

static void diagnostic_ping_success(
    esp_ping_handle_t handle,
    void *args)
{
    ping_result_t *result = (ping_result_t *)args;

    uint32_t elapsed = 0;

    if (esp_ping_get_profile(
            handle,
            ESP_PING_PROF_TIMEGAP,
            &elapsed,
            sizeof(elapsed)) == ESP_OK) {

        result->received++;
        result->total_rtt_ms += elapsed;
    }
}

static void diagnostic_ping_timeout(
    esp_ping_handle_t handle,
    void *args)
{
    (void)handle;
    (void)args;
}

static void diagnostic_ping_end(
    esp_ping_handle_t handle,
    void *args)
{
    ping_result_t *result = (ping_result_t *)args;

    esp_ping_get_profile(
        handle,
        ESP_PING_PROF_REQUEST,
        &result->transmitted,
        sizeof(result->transmitted)
    );

    esp_ping_get_profile(
        handle,
        ESP_PING_PROF_DURATION,
        &result->duration_ms,
        sizeof(result->duration_ms)
    );

    if (result->received > 0) {
        result->average_rtt_ms =
            result->total_rtt_ms / result->received;
    }

    result->completed = true;

    xSemaphoreGive(result->finished);
}

/* ---------------------------------------------------------
   PING HELPER
--------------------------------------------------------- */

static bool run_ping(
    ip_addr_t target,
    ping_result_t *result)
{
    memset(result, 0, sizeof(*result));

    result->finished = xSemaphoreCreateBinary();

    if (!result->finished) {
        ESP_LOGE(TAG, "Unable to create ping semaphore");
        return false;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();

    config.target_addr = target;
    config.count = 2;
    config.interval_ms = 300;
    config.timeout_ms = 800;
    config.data_size = 32;

    esp_ping_callbacks_t callbacks = {
        .cb_args = result,
        .on_ping_success = diagnostic_ping_success,
        .on_ping_timeout = diagnostic_ping_timeout,
        .on_ping_end = diagnostic_ping_end
    };

    esp_ping_handle_t session = NULL;

    esp_err_t err = esp_ping_new_session(
        &config,
        &callbacks,
        &session
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not create ping session: %s",
                 esp_err_to_name(err));

        vSemaphoreDelete(result->finished);
        result->finished = NULL;

        return false;
    }

    err = esp_ping_start(session);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start ping session: %s",
                 esp_err_to_name(err));

        esp_ping_delete_session(session);

        vSemaphoreDelete(result->finished);
        result->finished = NULL;

        return false;
    }

    /*
     * Two probes, each with an 800 ms timeout.
     * Allow extra time for task scheduling.
     */
    BaseType_t completed = xSemaphoreTake(
        result->finished,
        pdMS_TO_TICKS(5000)
    );

    if (completed != pdTRUE) {
        ESP_LOGW(TAG, "Ping session timed out");

        esp_ping_stop(session);

        /*
         * Allow the ping task to finish its current operation.
         */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_ping_delete_session(session);

    if (result->finished) {
        vSemaphoreDelete(result->finished);
        result->finished = NULL;
    }

    return completed == pdTRUE && result->completed;
}

/* ---------------------------------------------------------
   IP ADDRESS HELPER
--------------------------------------------------------- */

static void format_ip4(
    const esp_ip4_addr_t *address,
    char *buffer,
    size_t buffer_size)
{
    if (!address || !buffer || buffer_size == 0) {
        return;
    }

    snprintf(
        buffer,
        buffer_size,
        IPSTR,
        IP2STR(address)
    );
}

/* ---------------------------------------------------------
   DIAGNOSTICS API
--------------------------------------------------------- */

static esp_err_t diagnostics_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (!root) {
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(root, "connected", false);
    cJSON_AddBoolToObject(root, "dns_ok", false);
    cJSON_AddBoolToObject(root, "dns_resolved", false);
    cJSON_AddBoolToObject(root, "gateway_ping_ok", false);
    cJSON_AddBoolToObject(root, "internet_ping_ok", false);

    cJSON_AddStringToObject(root, "gateway", "Unavailable");
    cJSON_AddStringToObject(root, "dns_server", "Unavailable");
    cJSON_AddStringToObject(root, "resolved_ip", "Unavailable");

    cJSON_AddNumberToObject(root, "gateway_ping_received", 0);
    cJSON_AddNumberToObject(root, "gateway_ping_average_ms", 0);

    cJSON_AddNumberToObject(root, "internet_ping_received", 0);
    cJSON_AddNumberToObject(root, "internet_ping_average_ms", 0);

    if (!diagnostic_sta_netif) {
        cJSON_AddStringToObject(root, "message",
                                "Station interface unavailable");
        goto send_response;
    }

    /* -----------------------------------------------------
       CHECK STATION IP CONFIGURATION
    ----------------------------------------------------- */

    esp_netif_ip_info_t ip_info = {0};

    esp_err_t ip_result = esp_netif_get_ip_info(
        diagnostic_sta_netif,
        &ip_info
    );

    if (ip_result != ESP_OK || ip_info.ip.addr == 0) {
        cJSON_AddStringToObject(
            root,
            "message",
            "ESP32 is not connected to a router"
        );

        goto send_response;
    }

    cJSON_ReplaceItemInObject(
        root,
        "connected",
        cJSON_CreateBool(true)
    );

    char gateway_string[16] = "Unavailable";

    format_ip4(
        &ip_info.gw,
        gateway_string,
        sizeof(gateway_string)
    );

    cJSON_ReplaceItemInObject(
        root,
        "gateway",
        cJSON_CreateString(gateway_string)
    );

    /* -----------------------------------------------------
       GET DNS SERVER
    ----------------------------------------------------- */

    esp_netif_dns_info_t dns_info = {0};

    esp_err_t dns_info_result = esp_netif_get_dns_info(
        diagnostic_sta_netif,
        ESP_NETIF_DNS_MAIN,
        &dns_info
    );

    char dns_string[16] = "Unavailable";

    if (dns_info_result == ESP_OK &&
        dns_info.ip.type == ESP_IPADDR_TYPE_V4) {

        format_ip4(
            &dns_info.ip.u_addr.ip4,
            dns_string,
            sizeof(dns_string)
        );

        cJSON_ReplaceItemInObject(
            root,
            "dns_server",
            cJSON_CreateString(dns_string)
        );
    }

    /* -----------------------------------------------------
       GATEWAY PING
    ----------------------------------------------------- */

    if (ip_info.gw.addr != 0) {

        ip_addr_t gateway_target;

        memset(&gateway_target, 0, sizeof(gateway_target));

        IP_ADDR4(
            &gateway_target,
            ip4_addr1(&ip_info.gw),
            ip4_addr2(&ip_info.gw),
            ip4_addr3(&ip_info.gw),
            ip4_addr4(&ip_info.gw)
        );

        ping_result_t gateway_result = {0};

        bool gateway_completed = run_ping(
            gateway_target,
            &gateway_result
        );

        if (gateway_completed) {

            cJSON_ReplaceItemInObject(
                root,
                "gateway_ping_ok",
                cJSON_CreateBool(gateway_result.received > 0)
            );

            cJSON_ReplaceItemInObject(
                root,
                "gateway_ping_received",
                cJSON_CreateNumber(gateway_result.received)
            );

            cJSON_ReplaceItemInObject(
                root,
                "gateway_ping_average_ms",
                cJSON_CreateNumber(gateway_result.average_rtt_ms)
            );
        }
    }

    /* -----------------------------------------------------
       DNS RESOLUTION
    ----------------------------------------------------- */

    struct addrinfo hints = {0};
    struct addrinfo *resolved = NULL;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int dns_result = getaddrinfo(
        "example.com",
        NULL,
        &hints,
        &resolved
    );

    if (dns_result == 0 && resolved != NULL) {

        struct sockaddr_in *address =
            (struct sockaddr_in *)resolved->ai_addr;

        char resolved_string[16] = "Unavailable";

        snprintf(
            resolved_string,
            sizeof(resolved_string),
            IPSTR,
            IP2STR((esp_ip4_addr_t *)&address->sin_addr)
        );

        cJSON_ReplaceItemInObject(
            root,
            "dns_ok",
            cJSON_CreateBool(true)
        );

        cJSON_ReplaceItemInObject(
            root,
            "dns_resolved",
            cJSON_CreateBool(true)
        );

        cJSON_ReplaceItemInObject(
            root,
            "resolved_ip",
            cJSON_CreateString(resolved_string)
        );

        ESP_LOGI(TAG, "DNS resolution successful: %s",
                 resolved_string);

        freeaddrinfo(resolved);

    } else {
        ESP_LOGW(TAG, "DNS resolution failed: %d", dns_result);
    }

    /* -----------------------------------------------------
       INTERNET PING
    ----------------------------------------------------- */

    ip_addr_t internet_target;

    memset(&internet_target, 0, sizeof(internet_target));

    IP_ADDR4(&internet_target, 1, 1, 1, 1);

    ping_result_t internet_result = {0};

    bool internet_completed = run_ping(
        internet_target,
        &internet_result
    );

    if (internet_completed) {

        cJSON_ReplaceItemInObject(
            root,
            "internet_ping_ok",
            cJSON_CreateBool(internet_result.received > 0)
        );

        cJSON_ReplaceItemInObject(
            root,
            "internet_ping_received",
            cJSON_CreateNumber(internet_result.received)
        );

        cJSON_ReplaceItemInObject(
            root,
            "internet_ping_average_ms",
            cJSON_CreateNumber(internet_result.average_rtt_ms)
        );
    }

    if (dns_result == 0 && internet_result.received > 0) {
        cJSON_AddStringToObject(
            root,
            "message",
            "DNS resolution and external ping succeeded"
        );
    } else if (dns_result == 0) {
        cJSON_AddStringToObject(
            root,
            "message",
            "DNS works, but external ICMP ping did not receive a reply"
        );
    } else {
        cJSON_AddStringToObject(
            root,
            "message",
            "DNS resolution failed; check router and DNS settings"
        );
    }

send_response:

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char *json = cJSON_PrintUnformatted(root);

    if (!json) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    esp_err_t result = httpd_resp_sendstr(req, json);

    free(json);
    cJSON_Delete(root);

    return result;
}

/* ---------------------------------------------------------
   REGISTER ROUTE
--------------------------------------------------------- */

esp_err_t register_diagnostics_routes(
    httpd_handle_t server,
    esp_netif_t *sta_netif)
{
    if (!server || !sta_netif) {
        return ESP_ERR_INVALID_ARG;
    }

    diagnostic_sta_netif = sta_netif;

    httpd_uri_t diagnostics_uri = {
        .uri = "/api/diagnostics",
        .method = HTTP_GET,
        .handler = diagnostics_handler,
        .user_ctx = NULL
    };

    esp_err_t result = httpd_register_uri_handler(
        server,
        &diagnostics_uri
    );

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Diagnostics endpoint registered");
    } else {
        ESP_LOGE(TAG, "Could not register diagnostics endpoint");
    }

    return result;
}