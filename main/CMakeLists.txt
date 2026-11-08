idf_component_register(
    SRCS
        "main.c"
        "diagnostics.c"
        "file_sharing.c"

    INCLUDE_DIRS
        "."

    EMBED_TXTFILES
        "index.html"

    PRIV_REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_server
        esp_timer
        esp_system
        json
        lwip
        littlefs
        mbedtls
)