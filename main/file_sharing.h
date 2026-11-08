#ifndef FILE_SHARING_H
#define FILE_SHARING_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t file_sharing_init(void);

esp_err_t register_file_sharing_routes(
    httpd_handle_t server
);

#endif