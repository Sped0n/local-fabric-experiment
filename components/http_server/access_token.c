#include "esp_random.h"
#include "fabric_http.h"
#include "nvs.h"
#include "web_routes.h"
#include <string.h>
#include "freertos/FreeRTOS.h"

// The HTTP task is the sole writer after startup. UART reads a copy under the lock.
static portMUX_TYPE token_lock = portMUX_INITIALIZER_UNLOCKED;
static char current[FABRIC_HTTP_AUTHORIZATION_SIZE];

void fabric_http_copy_authorization(char out[FABRIC_HTTP_AUTHORIZATION_SIZE])
{
    portENTER_CRITICAL(&token_lock);
    memcpy(out, current, sizeof(current));
    portEXIT_CRITICAL(&token_lock);
}

static esp_err_t load_or_rotate(bool rotate)
{
    uint8_t bytes[16];
    nvs_handle_t store;
    esp_err_t err = nvs_open("fabric_http", NVS_READWRITE, &store);
    if (err != ESP_OK)
        return err;
    size_t length = sizeof(bytes);
    if (!rotate) {
        err = nvs_get_blob(store, "token", bytes, &length);
        if (err == ESP_OK && length != sizeof(bytes))
            err = ESP_ERR_INVALID_SIZE;
    }
    if (rotate || err == ESP_ERR_NVS_NOT_FOUND) {
        esp_fill_random(bytes, sizeof(bytes));
        err = nvs_set_blob(store, "token", bytes, sizeof(bytes));
        if (err == ESP_OK)
            err = nvs_commit(store);
    }
    nvs_close(store);
    if (err != ESP_OK)
        return err;

    static const char hex[] = "0123456789abcdef";
    char next[FABRIC_HTTP_AUTHORIZATION_SIZE] = "Bearer ";
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        next[7 + 2 * i] = hex[bytes[i] >> 4];
        next[8 + 2 * i] = hex[bytes[i] & 15];
    }
    portENTER_CRITICAL(&token_lock);
    memcpy(current, next, sizeof(current));
    portEXIT_CRITICAL(&token_lock);
    return ESP_OK;
}

bool fabric_http_authorized(httpd_req_t *req)
{
    char value[FABRIC_HTTP_AUTHORIZATION_SIZE];
    if (httpd_req_get_hdr_value_len(req, "Authorization") == sizeof(value) - 1 &&
        httpd_req_get_hdr_value_str(req, "Authorization", value, sizeof(value)) == ESP_OK) {
        unsigned diff = 0;
        // Both buffers are fixed length; don't stop at the first incorrect byte.
        for (size_t i = 0; i < sizeof(value) - 1; i++) diff |= (unsigned char)value[i] ^ (unsigned char)current[i];
        if (!diff)
            return true;
    }
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"Local Fabric\"");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "board access token required");
    return false;
}

bool fabric_http_origin_ok(httpd_req_t *r)
{
    if (!httpd_req_get_hdr_value_len(r, "Origin"))
        return true;
    char origin[128], host[112];
    return httpd_req_get_hdr_value_str(r, "Origin", origin, sizeof(origin)) == ESP_OK &&
        httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) == ESP_OK && !strncmp(origin, "http://", 7) &&
        !strcasecmp(origin + 7, host);
}

static esp_err_t rotate_token(httpd_req_t *req)
{
    if (!fabric_http_origin_ok(req))
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "invalid Origin");
    if (!fabric_http_authorized(req))
        return ESP_OK;
    if (req->content_len)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected an empty body");
    esp_err_t err = load_or_rotate(true);
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "token was not committed");
    fabric_http_revoke_streams();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, current + 7);
}

esp_err_t fabric_http_access_init(void)
{
    return load_or_rotate(false);
}
esp_err_t fabric_http_settings_start(httpd_handle_t server)
{
    const httpd_uri_t uri = {.uri = "/settings/token/rotate", .method = HTTP_POST, .handler = rotate_token};
    return httpd_register_uri_handler(server, &uri);
}
