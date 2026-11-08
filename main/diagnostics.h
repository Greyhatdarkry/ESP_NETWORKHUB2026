#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include "esp_http_server.h"
#include "esp_netif.h"

esp_err_t register_diagnostics_routes(
    httpd_handle_t server,
    esp_netif_t *sta_netif
);

#endif