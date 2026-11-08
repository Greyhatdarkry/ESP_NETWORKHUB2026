
from pathlib import Path
from datetime import datetime
import re
import sys

PROJECT = Path(__file__).resolve().parent
MAIN_C = PROJECT / "main" / "main.c"

if not MAIN_C.exists():
    sys.exit(f"ERROR: Could not find {MAIN_C}")

source = MAIN_C.read_text(encoding="utf-8")

start_marker = "static httpd_handle_t start_webserver(void)"
end_marker = "/* ---------------------------------------------------------\n   WIFI INITIALIZATION"

if source.count(start_marker) != 1:
    sys.exit("ERROR: Expected exactly one start_webserver() function.")

if source.count(end_marker) != 1:
    sys.exit("ERROR: Could not uniquely locate WIFI INITIALIZATION.")

start = source.index(start_marker)
end = source.index(end_marker, start)

replacement = r'''static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.server_port = 80;
    config.max_uri_handlers = 20;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    esp_err_t result = httpd_start(&server, &config);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Unable to start HTTP server: %s",
                 esp_err_to_name(result));
        server = NULL;
        return NULL;
    }

    httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/clients", .method = HTTP_GET, .handler = clients_handler },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler },
        { .uri = "/api/connect", .method = HTTP_POST, .handler = connect_handler },
        { .uri = "/api/disconnect", .method = HTTP_POST, .handler = disconnect_handler },
        { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_handler }
    };

    const size_t route_count = sizeof(routes) / sizeof(routes[0]);

    /* Register dashboard routes exactly once. */
    for (size_t i = 0; i < route_count; i++) {
        result = httpd_register_uri_handler(server, &routes[i]);

        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Dashboard route registration failed: %s (%s)",
                     routes[i].uri, esp_err_to_name(result));
            goto registration_failed;
        }
    }

    /* Register file-sharing routes exactly once, outside the loop. */
    result = register_file_sharing_routes(server);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "File-sharing route registration failed: %s",
                 esp_err_to_name(result));
        goto registration_failed;
    }

    /* Register diagnostics exactly once. */
    result = register_diagnostics_routes(server, sta_netif);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Diagnostics route registration failed: %s",
                 esp_err_to_name(result));
        goto registration_failed;
    }

    ESP_LOGI(TAG, "All HTTP routes registered successfully");
    ESP_LOGI(TAG, "Web server started");

    return server;

registration_failed:
    httpd_stop(server);
    server = NULL;
    return NULL;
}

'''

updated = source[:start] + replacement + source[end:]

backup = MAIN_C.with_name(
    f"main.c.backup_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
)

backup.write_text(source, encoding="utf-8")
MAIN_C.write_text(updated, encoding="utf-8")

# Verify the new function contains only one registration call.
function_start = updated.index(start_marker)
function_end = updated.index(end_marker, function_start)
new_function = updated[function_start:function_end]

if new_function.count("register_file_sharing_routes(server)") != 1:
    MAIN_C.write_text(source, encoding="utf-8")
    sys.exit("ERROR: Verification failed. Original main.c restored.")

if new_function.count("register_diagnostics_routes(server, sta_netif)") != 1:
    MAIN_C.write_text(source, encoding="utf-8")
    sys.exit("ERROR: Diagnostics verification failed. Original main.c restored.")

print("SUCCESS: start_webserver() was corrected.")
print(f"Backup: {backup}")
print("File-sharing registration count: 1")
print("Diagnostics registration count: 1")
print("No flash operation or storage formatting was performed.")
