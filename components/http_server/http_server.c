#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "fabric_http.h"
#include "web_routes.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"

static fabric_core_handle_t core;
static fabric_manager_handle_t manager;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    bool pending;
    esp_err_t result;
    bool used;
} operation_t;
static operation_t membership, control;

static const char *state(operation_t op)
{
    return !op.used ? "idle" : op.pending ? "pending" : esp_err_to_name(op.result);
}

char *http_status_json(void)
{
    char body[256];
    portENTER_CRITICAL(&lock);
    operation_t m = membership, c = control;
    portEXIT_CRITICAL(&lock);
    snprintf(body, sizeof(body),
             "{\"uptime_s\":%lld,\"free_internal_bytes\":%u,\"membership\":\"%s\",\"control\":\"%s\"}",
             (long long)(esp_timer_get_time() / 1000000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), state(m), state(c));
    return strdup(body);
}

static void completed(fabric_core_handle_t unused, const fabric_core_request_result_t *result, void *ctx)
{
    operation_t *op = ctx;
    portENTER_CRITICAL(&lock);
    op->pending = false;
    op->result = result->error;
    portEXIT_CRITICAL(&lock);
    fabric_http_status_changed();
}

static uint32_t number(const cJSON *obj, const char *name, uint32_t max, bool *valid)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= max) ||
        v->valuedouble != trunc(v->valuedouble)) {
        *valid = false;
        return 0;
    }
    return (uint32_t)v->valuedouble;
}

esp_err_t http_submit_control(const char *route, const cJSON *obj)
{
    if (!strcmp(route, "/cancel"))
        return fabric_core_cancel_commissioning(core);
    operation_t *op = strcmp(route, "/action") == 0 ? &control : &membership;
    portENTER_CRITICAL(&lock);
    bool busy = op->pending;
    if (!busy)
        *op = (operation_t){.used = true, .pending = true};
    portEXIT_CRITICAL(&lock);
    if (busy)
        return ESP_ERR_INVALID_STATE;
    fabric_http_status_changed();
    esp_err_t err = ESP_ERR_INVALID_ARG;
    bool valid = true;
    if (!strcmp(route, "/commission")) {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "code"));
        if (code)
            err = fabric_core_start_commissioning(core, code, completed, op);
    } else {
        uint32_t id = number(obj, "device_id", UINT32_MAX, &valid);
        if (op == &membership && valid)
            err = fabric_manager_remove_device(manager, id, false, completed, op);
        if (op == &control) {
            uint16_t ep = number(obj, "endpoint_id", UINT16_MAX, &valid);
            const char *cluster = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "cluster"));
            const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "action"));
            cJSON *args = cJSON_GetObjectItemCaseSensitive(obj, "arguments");
            if (valid && cluster && action && cJSON_IsObject(args)) {
                char *payload = cJSON_PrintUnformatted(args);
                err = payload ? fabric_manager_invoke_action(manager, id, ep, cluster, action, payload, completed, op)
                              : ESP_ERR_NO_MEM;
                cJSON_free(payload);
            }
        }
    }
    if (err != ESP_OK) {
        portENTER_CRITICAL(&lock);
        op->result = err;
        op->pending = false;
        portEXIT_CRITICAL(&lock);
        fabric_http_status_changed();
        return err;
    }
    return ESP_OK;
}

esp_err_t fabric_http_start(fabric_core_handle_t c, fabric_manager_handle_t m, fabric_automation_t automation)
{
    core = c;
    manager = m;
    ESP_RETURN_ON_ERROR(fabric_http_access_init(), "http", "load board access token");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 16384;
    config.max_uri_handlers = 7;
    config.lru_purge_enable = true;
    config.max_open_sockets = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), "http", "start");
    esp_err_t err = fabric_http_web_start(server, manager);
    if (err == ESP_OK)
        err = fabric_http_settings_start(server);
    if (err == ESP_OK)
        err = fabric_http_mcp_start(server, manager, automation);
    if (err != ESP_OK) {
        httpd_stop(server);
        return err;
    }
    return ESP_OK;
}
