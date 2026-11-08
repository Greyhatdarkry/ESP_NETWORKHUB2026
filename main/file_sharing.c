#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "cJSON.h"

#define RAM_STORAGE_TOTAL_BYTES  (64U * 1024U)
#define RAM_STORAGE_MAX_FILE     (32U * 1024U)
#define RAM_STORAGE_MAX_FILES    10
#define STORAGE_CHUNK_SIZE       2048
#define PASSWORD_MIN_LENGTH      8
#define PASSWORD_MAX_LENGTH      128
#define PASSWORD_SALT_SIZE       16
#define PASSWORD_HASH_SIZE       32
#define PASSWORD_ITERATIONS      10000

static const char *TAG = "FILE_SHARING";

typedef struct {
    char name[65];
    uint8_t *data;
    size_t size;
    uint8_t salt[PASSWORD_SALT_SIZE];
    uint8_t password_hash[PASSWORD_HASH_SIZE];
    bool occupied;
} ram_file_t;

static ram_file_t files[RAM_STORAGE_MAX_FILES];
static bool storage_ready;
static SemaphoreHandle_t storage_mutex;

static bool lock_storage(void)
{
    return storage_mutex &&
           xSemaphoreTake(storage_mutex, pdMS_TO_TICKS(5000)) == pdTRUE;
}

static void unlock_storage(void)
{
    if (storage_mutex) {
        xSemaphoreGive(storage_mutex);
    }
}

static bool hash_password(const char *password,
                          const uint8_t *salt,
                          uint8_t *output)
{
    int result = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        (const unsigned char *)password,
        strlen(password),
        salt,
        PASSWORD_SALT_SIZE,
        PASSWORD_ITERATIONS,
        PASSWORD_HASH_SIZE,
        output
    );

    return result == 0;
}

static bool password_matches(const char *password,
                             const ram_file_t *file)
{
    uint8_t calculated[PASSWORD_HASH_SIZE];

    if (!hash_password(password, file->salt, calculated)) {
        return false;
    }

    uint8_t difference = 0;
    for (size_t i = 0; i < PASSWORD_HASH_SIZE; i++) {
        difference |= calculated[i] ^ file->password_hash[i];
    }

    memset(calculated, 0, sizeof(calculated));
    return difference == 0;
}

static bool get_password_header(httpd_req_t *req,
                                char *password,
                                size_t password_size)
{
    size_t length =
        httpd_req_get_hdr_value_len(req, "X-File-Password");

    if (length < PASSWORD_MIN_LENGTH ||
        length > PASSWORD_MAX_LENGTH ||
        length >= password_size) {
        return false;
    }

    if (httpd_req_get_hdr_value_str(
            req, "X-File-Password",
            password, password_size) != ESP_OK) {
        return false;
    }

    size_t actual = strlen(password);
    return actual >= PASSWORD_MIN_LENGTH &&
           actual <= PASSWORD_MAX_LENGTH;
}

static size_t storage_used_bytes_locked(void)
{
    size_t used = 0;

    for (size_t i = 0; i < RAM_STORAGE_MAX_FILES; i++) {
        if (files[i].occupied) used += files[i].size;
    }

    return used;
}

static int find_file_locked(const char *name)
{
    for (size_t i = 0; i < RAM_STORAGE_MAX_FILES; i++) {
        if (files[i].occupied &&
            strcmp(files[i].name, name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int find_free_slot_locked(void)
{
    for (size_t i = 0; i < RAM_STORAGE_MAX_FILES; i++) {
        if (!files[i].occupied) return (int)i;
    }

    return -1;
}

static bool storage_available(httpd_req_t *req)
{
    if (storage_ready && storage_mutex) return true;

    httpd_resp_send_err(
        req, HTTPD_500_INTERNAL_SERVER_ERROR,
        "RAM storage is unavailable"
    );
    return false;
}

static bool valid_filename(const char *name)
{
    if (!name || !name[0]) return false;

    size_t length = strlen(name);
    if (length > 64 || name[0] == '.') return false;

    for (size_t i = 0; i < length; i++) {
        char c = name[i];

        bool valid =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-';

        if (!valid) return false;
    }

    return true;
}

static bool get_filename(httpd_req_t *req,
                         char *filename,
                         size_t filename_size)
{
    char query[128] = {0};

    if (httpd_req_get_url_query_str(
            req, query, sizeof(query)) != ESP_OK) {
        return false;
    }

    if (httpd_query_key_value(
            query, "name", filename,
            filename_size) != ESP_OK) {
        return false;
    }

    return valid_filename(filename);
}

static esp_err_t send_json_message(httpd_req_t *req,
                                   const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(
            req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "JSON allocation failed"
        );
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", message);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) return ESP_FAIL;

    httpd_resp_set_type(req, "application/json");
    esp_err_t result = httpd_resp_sendstr(req, json);
    free(json);
    return result;
}

esp_err_t file_sharing_init(void)
{
    if (!storage_mutex) {
        storage_mutex = xSemaphoreCreateMutex();
    }

    if (!storage_mutex) {
        ESP_LOGE(TAG, "Unable to create storage mutex");
        return ESP_ERR_NO_MEM;
    }

    if (!lock_storage()) return ESP_FAIL;

    for (size_t i = 0; i < RAM_STORAGE_MAX_FILES; i++) {
        if (files[i].data) free(files[i].data);
        memset(&files[i], 0, sizeof(files[i]));
    }

    storage_ready = true;
    unlock_storage();

    ESP_LOGI(TAG, "RAM file storage initialized");
    ESP_LOGI(TAG, "Storage capacity: %u bytes",
             (unsigned)RAM_STORAGE_TOTAL_BYTES);
    ESP_LOGI(TAG, "Maximum file size: %u bytes",
             (unsigned)RAM_STORAGE_MAX_FILE);
    ESP_LOGI(TAG, "Maximum files: %u",
             (unsigned)RAM_STORAGE_MAX_FILES);
    ESP_LOGW(TAG, "RAM files are erased when the ESP32 restarts");

    return ESP_OK;
}

/* GET /api/storage */
static esp_err_t storage_status_handler(httpd_req_t *req)
{
    if (!storage_available(req)) return ESP_FAIL;
    if (!lock_storage()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal server error");

    size_t used = storage_used_bytes_locked();
    unlock_storage();

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal server error");

    cJSON_AddBoolToObject(root, "available", true);
    cJSON_AddNumberToObject(root, "total", RAM_STORAGE_TOTAL_BYTES);
    cJSON_AddNumberToObject(root, "used", used);
    cJSON_AddNumberToObject(root, "free",
                            RAM_STORAGE_TOTAL_BYTES - used);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal server error");

    httpd_resp_set_type(req, "application/json");
    esp_err_t result = httpd_resp_sendstr(req, json);
    free(json);
    return result;
}

/* GET /api/files */
static esp_err_t file_list_handler(httpd_req_t *req)
{
    if (!storage_available(req)) return ESP_FAIL;
    if (!lock_storage()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal server error");

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();

    if (!root || !array) {
        cJSON_Delete(root);
        cJSON_Delete(array);
        unlock_storage();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal server error");
    }

    for (size_t i = 0; i < RAM_STORAGE_MAX_FILES; i++) {
        if (!files[i].occupied) continue;

        cJSON *entry = cJSON_CreateObject();
        if (!entry) continue;

        cJSON_AddStringToObject(entry, "name", files[i].name);
        cJSON_AddNumberToObject(entry, "size",
                                (double)files[i].size);
        cJSON_AddBoolToObject(entry, "protected", true);
        cJSON_AddItemToArray(array, entry);
    }

    unlock_storage();

    cJSON_AddItemToObject(root, "files", array);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal server error");

    httpd_resp_set_type(req, "application/json");
    esp_err_t result = httpd_resp_sendstr(req, json);
    free(json);
    return result;
}

/* POST /api/files/upload?name=filename
   Header: X-File-Password */
static esp_err_t file_upload_handler(httpd_req_t *req)
{
    if (!storage_available(req)) return ESP_FAIL;

    char filename[65] = {0};
    char password[PASSWORD_MAX_LENGTH + 1] = {0};

    if (!get_filename(req, filename, sizeof(filename))) {
        httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "Invalid filename. Use letters, numbers, dot, dash or underscore."
        );
        return ESP_FAIL;
    }

    if (!get_password_header(req, password, sizeof(password))) {
        httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "File password must be 8 to 128 characters"
        );
        return ESP_FAIL;
    }

    if (req->content_len <= 0 ||
        (size_t)req->content_len > RAM_STORAGE_MAX_FILE) {
        memset(password, 0, sizeof(password));
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "Maximum file size is 32 KB");
        return ESP_FAIL;
    }

    size_t new_size = (size_t)req->content_len;
    uint8_t *new_data = malloc(new_size);

    if (!new_data) {
        memset(password, 0, sizeof(password));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal server error");
    }

    size_t received_total = 0;

    while (received_total < new_size) {
        size_t amount = new_size - received_total;
        if (amount > STORAGE_CHUNK_SIZE) {
            amount = STORAGE_CHUNK_SIZE;
        }

        int received = httpd_req_recv(
            req,
            (char *)new_data + received_total,
            amount
        );

        if (received <= 0) {
            free(new_data);
            memset(password, 0, sizeof(password));
            httpd_resp_send_err(
                req, HTTPD_500_INTERNAL_SERVER_ERROR,
                "Upload reception failed"
            );
            return ESP_FAIL;
        }

        received_total += (size_t)received;
    }

    ram_file_t candidate = {0};
    strncpy(candidate.name, filename, sizeof(candidate.name) - 1);
    candidate.data = new_data;
    candidate.size = new_size;
    candidate.occupied = true;

    esp_fill_random(candidate.salt, sizeof(candidate.salt));

    bool hash_ok = hash_password(
        password, candidate.salt, candidate.password_hash
    );
    memset(password, 0, sizeof(password));

    if (!hash_ok) {
        free(new_data);
        httpd_resp_send_err(
            req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "Password hashing failed"
        );
        return ESP_FAIL;
    }

    if (!lock_storage()) {
        free(new_data);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal server error");
    }

    int existing = find_file_locked(filename);
    int target = existing;

    if (existing < 0) target = find_free_slot_locked();

    if (target < 0 ||
        storage_used_bytes_locked() -
            (existing >= 0 ? files[existing].size : 0) +
            new_size > RAM_STORAGE_TOTAL_BYTES) {
        unlock_storage();
        free(new_data);

        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(
            req, "Storage is full or maximum file count reached"
        );
        return ESP_FAIL;
    }

    if (existing >= 0) free(files[target].data);
    files[target] = candidate;

    unlock_storage();

    ESP_LOGI(TAG, "Uploaded protected file: %s (%u bytes)",
             filename, (unsigned)new_size);

    return send_json_message(req, "Protected file uploaded successfully");
}

/* GET /api/files/download?name=filename
   Header: X-File-Password */
static esp_err_t file_download_handler(httpd_req_t *req)
{
    if (!storage_available(req)) return ESP_FAIL;

    char filename[65] = {0};
    char password[PASSWORD_MAX_LENGTH + 1] = {0};

    if (!get_filename(req, filename, sizeof(filename))) {
        httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST, "Invalid filename"
        );
        return ESP_FAIL;
    }

    if (!get_password_header(req, password, sizeof(password))) {
        httpd_resp_send_err(
            req, HTTPD_401_UNAUTHORIZED,
            "A file password is required"
        );
        return ESP_FAIL;
    }

    if (!lock_storage()) {
        memset(password, 0, sizeof(password));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal server error");
    }

    int index = find_file_locked(filename);

    if (index < 0) {
        unlock_storage();
        memset(password, 0, sizeof(password));
        httpd_resp_send_err(
            req, HTTPD_404_NOT_FOUND, "File not found"
        );
        return ESP_FAIL;
    }

    bool authorized = password_matches(password, &files[index]);
    memset(password, 0, sizeof(password));

    if (!authorized) {
        unlock_storage();
        httpd_resp_send_err(
            req, HTTPD_401_UNAUTHORIZED,
            "Incorrect file password"
        );
        return ESP_FAIL;
    }

    char disposition[128];
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"%s\"", filename);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    size_t offset = 0;
    esp_err_t result = ESP_OK;

    while (offset < files[index].size) {
        size_t amount = files[index].size - offset;
        if (amount > STORAGE_CHUNK_SIZE) {
            amount = STORAGE_CHUNK_SIZE;
        }

        result = httpd_resp_send_chunk(
            req,
            (const char *)files[index].data + offset,
            amount
        );

        if (result != ESP_OK) break;
        offset += amount;
    }

    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(req, NULL, 0);
    }

    unlock_storage();
    return result;
}

/* POST /api/files/delete?name=filename
   Header: X-File-Password */
static esp_err_t file_delete_handler(httpd_req_t *req)
{
    if (!storage_available(req)) return ESP_FAIL;

    char filename[65] = {0};
    char password[PASSWORD_MAX_LENGTH + 1] = {0};

    if (!get_filename(req, filename, sizeof(filename))) {
        httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST, "Invalid filename"
        );
        return ESP_FAIL;
    }

    if (!get_password_header(req, password, sizeof(password))) {
        httpd_resp_send_err(
            req, HTTPD_401_UNAUTHORIZED,
            "File password is required"
        );
        return ESP_FAIL;
    }

    if (!lock_storage()) {
        memset(password, 0, sizeof(password));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal server error");
    }

    int index = find_file_locked(filename);

    if (index < 0) {
        unlock_storage();
        memset(password, 0, sizeof(password));
        httpd_resp_send_err(
            req, HTTPD_404_NOT_FOUND, "File not found"
        );
        return ESP_FAIL;
    }

    bool authorized = password_matches(password, &files[index]);
    memset(password, 0, sizeof(password));

    if (!authorized) {
        unlock_storage();
        httpd_resp_send_err(
            req, HTTPD_401_UNAUTHORIZED,
            "Incorrect file password"
        );
        return ESP_FAIL;
    }

    free(files[index].data);
    memset(&files[index], 0, sizeof(files[index]));

    unlock_storage();

    ESP_LOGI(TAG, "Deleted protected file: %s", filename);
    return send_json_message(req, "File deleted successfully");
}

esp_err_t register_file_sharing_routes(httpd_handle_t server)
{
    if (!server) return ESP_ERR_INVALID_ARG;

    httpd_uri_t routes[] = {
        {
            .uri = "/api/storage",
            .method = HTTP_GET,
            .handler = storage_status_handler
        },
        {
            .uri = "/api/files",
            .method = HTTP_GET,
            .handler = file_list_handler
        },
        {
            .uri = "/api/files/upload",
            .method = HTTP_POST,
            .handler = file_upload_handler
        },
        {
            .uri = "/api/files/download",
            .method = HTTP_GET,
            .handler = file_download_handler
        },
        {
            .uri = "/api/files/delete",
            .method = HTTP_POST,
            .handler = file_delete_handler
        }
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t result =
            httpd_register_uri_handler(server, &routes[i]);

        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Route registration failed: %s (%s)",
                     routes[i].uri, esp_err_to_name(result));
            return result;
        }
    }

    ESP_LOGI(TAG, "Password-protected RAM file routes registered");
    return ESP_OK;
}
