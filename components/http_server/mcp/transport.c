#include "mcp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mbedtls/base64.h"

esp_err_t mcp_unavailable(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "MCP capacity exhausted");
}
static esp_err_t reply(httpd_req_t *r, const char *id, const char *key, const char *value)
{
    char *body = NULL;
    if (!value || asprintf(&body, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"%s\":%s}", id, key, value) < 0)
        return mcp_unavailable(r);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(r, body);
    free(body);
    return err;
}
esp_err_t mcp_result(httpd_req_t *r, const char *id, const char *result)
{
    return reply(r, id, "result", result);
}
esp_err_t mcp_error(httpd_req_t *r, const char *id, int code, const char *message)
{
    char error[180]; // Messages here are static, not request data.
    snprintf(error, sizeof(error), "{\"code\":%d,\"message\":\"%s\"}", code, message);
    httpd_resp_set_status(r, code == -32601 ? "404 Not Found" : "400 Bad Request");
    return reply(r, id, "error", error);
}
static bool matches(httpd_req_t *r, const char *header, const char *expected, bool encoded)
{
    char value[256];
    if (!expected || httpd_req_get_hdr_value_str(r, header, value, sizeof(value)) != ESP_OK)
        return false;
    if (encoded && !strncmp(value, "=?base64?", 9)) {
        size_t len = strlen(value), n = 0;
        unsigned char bytes[192];
        return len >= 11 && !strcmp(value + len - 2, "?=") &&
            mbedtls_base64_decode(bytes, sizeof(bytes), &n, (unsigned char *)value + 9, len - 11) == 0 &&
            n == strlen(expected) && !memcmp(bytes, expected, n);
    }
    return !strcmp(value, expected);
}
static const char *string(const cJSON *obj, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
}
static esp_err_t dispatch(httpd_req_t *r, const cJSON *obj, const char *id)
{
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(obj, "params"),
                *meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
    const char *method = string(obj, "method"), *version = string(meta, MCP_META "protocolVersion");
    if (!method || !string(obj, "jsonrpc") || strcmp(string(obj, "jsonrpc"), "2.0") || !id)
        return mcp_error(r, "null", -32600, "Invalid request");
    const char *key = !strcmp(method, "tools/call") ? "name" : !strcmp(method, "resources/read") ? "uri" : NULL;
    if (!matches(r, "MCP-Protocol-Version", version, false) || !matches(r, "Mcp-Method", method, false) ||
        (key && !matches(r, "Mcp-Name", string(params, key), true)))
        return mcp_error(r, id, -32020, "Missing or mismatched request header");
    if (strcmp(version, MCP_VERSION)) {
        char *requested = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(meta, MCP_META "protocolVersion")),
             *error = NULL;
        if (requested)
            asprintf(&error,
                     "{\"code\":-32022,\"message\":\"Unsupported protocol "
                     "version\",\"data\":{\"requested\":%s,\"supported\":[\"" MCP_VERSION "\"]}}",
                     requested);
        cJSON_free(requested);
        httpd_resp_set_status(r, "400 Bad Request");
        esp_err_t err = reply(r, id, "error", error);
        free(error);
        return err;
    }
    const cJSON *client = cJSON_GetObjectItemCaseSensitive(meta, MCP_META "clientInfo");
    if (!string(client, "name") || !string(client, "version") ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(meta, MCP_META "clientCapabilities")))
        return mcp_error(r, id, -32602, "Missing client metadata");
    if (!strcmp(method, "subscriptions/listen"))
        return mcp_subscriptions_listen(r, id, params);
    return mcp_catalog_request(r, id, method, params);
}
// Bound recursion before cJSON enters its parser on the HTTP server stack.
static bool shallow_json(const char *s, size_t len)
{
    unsigned depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\0')
            return false;
        if (quoted && c == '\\') {
            if (++i == len || !s[i])
                return false;
            continue;
        }
        if (c == '"')
            quoted = !quoted;
        else if (!quoted) {
            if ((c == '{' || c == '[') && ++depth > 24)
                return false;
            if ((c == '}' || c == ']') && depth)
                depth--;
        }
    }
    return true;
}
static esp_err_t route(httpd_req_t *r)
{
    if (!fabric_http_origin_ok(r)) {
        httpd_resp_set_status(r, "403 Forbidden");
        return httpd_resp_sendstr(r, "invalid Origin");
    }
    if (!fabric_http_authorized(r))
        return ESP_OK;
    if (r->method != HTTP_POST) {
        httpd_resp_set_hdr(r, "Allow", "POST");
        httpd_resp_set_status(r, "405 Method Not Allowed");
        return httpd_resp_sendstr(r, "POST required");
    }
    char ct[80];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", ct, sizeof(ct)) != ESP_OK ||
        strncasecmp(ct, "application/json", 16) || (ct[16] && ct[16] != ';')) {
        httpd_resp_set_status(r, "415 Unsupported Media Type");
        return httpd_resp_sendstr(r, "application/json required");
    }
    size_t len = r->content_len;
    if (len > 6144) {
        httpd_resp_set_status(r, "413 Content Too Large");
        return httpd_resp_sendstr(r, "6144 byte limit");
    }
    char *body = malloc(len + 1);
    if (!body)
        return mcp_unavailable(r);
    for (size_t off = 0; off < len;) {
        int n = httpd_req_recv(r, body + off, len - off);
        if (n <= 0) {
            free(body);
            return ESP_FAIL;
        }
        off += n;
    }
    body[len] = 0;
    cJSON *obj = shallow_json(body, len) ? cJSON_ParseWithOpts(body, NULL, true) : NULL;
    free(body);
    if (!obj)
        return mcp_error(r, "null", -32700, "Invalid JSON");
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, "id");
    bool valid = cJSON_IsString(item) ||
        (cJSON_IsNumber(item) && isfinite(item->valuedouble) && trunc(item->valuedouble) == item->valuedouble);
    char *id = valid ? cJSON_PrintUnformatted(item) : NULL;
    esp_err_t err = valid && !id ? mcp_unavailable(r) : dispatch(r, obj, id);
    cJSON_free(id);
    cJSON_Delete(obj);
    return err;
}
esp_err_t fabric_http_mcp_start(httpd_handle_t server, fabric_manager_handle_t manager, fabric_automation_t automation)
{
    mcp_catalog_init(manager, automation);
    const httpd_method_t methods[] = {HTTP_POST, HTTP_GET, HTTP_DELETE};
    for (size_t i = 0; i < 3; i++) {
        httpd_uri_t uri = {.uri = "/mcp", .method = methods[i], .handler = route};
        esp_err_t err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK)
            return err;
    }
    return mcp_subscriptions_start(manager, automation);
}
