#include "esp_timer.h"
#include "mcp.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

// A slot belongs to one async response, never to a client identity. The HTTP task
// admits streams and rotates credentials; a worker releases its slot before returning the request.
#define MAX_EXACT 8
typedef struct {
    fabric_property_path_t path;
    char *uri;
    cJSON *previous;
} exact_t;
typedef struct {
    exact_t *exact;
    unsigned count, devices, changed_devices;
    httpd_req_t *req;
    int fd;
    unsigned interests, dirty;
    char *id, *ack;
} stream_t;
static stream_t streams[6];
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static void changed(unsigned mask)
{
    portENTER_CRITICAL(&lock);
    for (size_t i = 0; i < 6; i++) streams[i].dirty |= streams[i].interests & mask;
    portEXIT_CRITICAL(&lock);
}
void fabric_http_status_changed(void)
{
    changed(1u << MCP_STATUS);
}
#if CONFIG_FABRIC_AUTOMATION
static void rules_changed(void)
{
    changed(1u << MCP_RULES);
}
#endif
static void devices_changed(void *ctx, unsigned views, uint32_t devices)
{
    unsigned mask =
        (views & FABRIC_DEVICE_METADATA ? 1u << MCP_METADATA : 0) | (views & FABRIC_DEVICE_STATE ? 1u << MCP_STATE : 0);
    portENTER_CRITICAL(&lock);
    for (size_t i = 0; i < 6; i++) {
        streams[i].dirty |= streams[i].interests & mask;
        if (views & FABRIC_DEVICE_STATE)
            streams[i].changed_devices |= streams[i].devices & devices;
    }
    portEXIT_CRITICAL(&lock);
}
esp_err_t mcp_watch_start(fabric_manager_handle_t manager, fabric_automation_t automation)
{
#if CONFIG_FABRIC_AUTOMATION
    if (automation)
        fabric_automation_set_changed_handler(automation, rules_changed);
#endif
    return fabric_manager_add_signal(manager, devices_changed, NULL);
}
static void clear_filter(stream_t *s)
{
    for (unsigned i = 0; i < s->count; i++) {
        free(s->exact[i].uri);
        cJSON_Delete(s->exact[i].previous);
    }
    free(s->exact);
    free(s->id);
    free(s->ack);
}
void fabric_http_revoke_streams(void)
{
    for (size_t i = 0; i < 6; i++) {
        portENTER_CRITICAL(&lock);
        int fd = streams[i].req ? streams[i].fd : -1;
        portEXIT_CRITICAL(&lock);
        if (fd >= 0)
            shutdown(fd, SHUT_RDWR);
    }
}
static void release_stream(stream_t *s)
{
    httpd_req_t *req = s->req;
    clear_filter(s);
    portENTER_CRITICAL(&lock);
    *s = (stream_t){0};
    portEXIT_CRITICAL(&lock);
    httpd_req_async_handler_complete(req);
}
static esp_err_t notify(stream_t *s, const char *method, const char *fields)
{
    char *event = NULL;
    if (asprintf(&event,
                 "event: message\ndata: "
                 "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/%s\",\"params\":{\"_meta\":{\"" MCP_META
                 "subscriptionId\":%s},%s}}\n\n",
                 method, s->id, fields) < 0)
        return ESP_ERR_NO_MEM;
    esp_err_t err = httpd_resp_sendstr_chunk(s->req, event);
    free(event);
    return err;
}
static void stream_loop(void *arg)
{
    stream_t *s = arg;
    httpd_resp_set_type(s->req, "text/event-stream");
    httpd_resp_set_hdr(s->req, "Cache-Control", "no-store");
    esp_err_t err = notify(s, "subscriptions/acknowledged", s->ack);
    unsigned ticks = 0;
    while (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        portENTER_CRITICAL(&lock);
        unsigned devices = s->changed_devices;
        s->changed_devices = 0;
        unsigned dirty = s->dirty;
        s->dirty = 0;
        portEXIT_CRITICAL(&lock);
        if (ticks % 50 == 49)
            dirty |= s->interests & (1u << MCP_STATUS);
        for (unsigned i = 0; i < s->count && err == ESP_OK; i++) {
            exact_t *e = &s->exact[i];
            if (!(devices & (1u << (e->path.device % 32))))
                continue;
            cJSON *now = NULL;
            esp_err_t read = mcp_get_property(&e->path, &now);
            if (read != ESP_OK && read != ESP_ERR_NOT_FOUND) {
                err = read;
                break;
            }
            if ((now || e->previous) && !cJSON_Compare(now, e->previous, true)) {
                char field[210];
                snprintf(field, sizeof(field), "\"uri\":\"%s\"", e->uri);
                err = notify(s, "resources/updated", field);
            }
            cJSON_Delete(e->previous);
            e->previous = now;
        }
        for (int i = 0; i < MCP_RESOURCE_COUNT && err == ESP_OK; i++)
            if (dirty & (1u << i)) {
                char uri[64];
                snprintf(uri, sizeof(uri), "\"uri\":\"%s\"", mcp_resource_uris[i]);
                err = notify(s, "resources/updated", uri);
            }
        if (err == ESP_OK && ++ticks % 20 == 0) {
            err = httpd_resp_sendstr_chunk(s->req, ": heartbeat\n\n");
        }
    }
    shutdown(s->fd, SHUT_RDWR);
    release_stream(s);
    vTaskDelete(NULL);
}
esp_err_t mcp_listen(httpd_req_t *r, const char *id, const cJSON *params)
{
    const cJSON *filter = cJSON_GetObjectItemCaseSensitive(params, "notifications"),
                *uris = cJSON_GetObjectItemCaseSensitive(filter, "resourceSubscriptions");
    if (!cJSON_IsObject(filter) || (uris && !cJSON_IsArray(uris)))
        return mcp_error(r, id, -32602, "Invalid notifications filter");
    stream_t *s = NULL;
    portENTER_CRITICAL(&lock);
    for (unsigned i = 0; i < 6; i++)
        if (!streams[i].req) {
            s = &streams[i];
            break;
        }
    portEXIT_CRITICAL(&lock);
    if (!s)
        return mcp_unavailable(r);
    stream_t slot = {.fd = httpd_req_to_sockfd(r), .id = strdup(id)};
    cJSON *filter_ack = cJSON_CreateObject(), *accepted = cJSON_CreateArray();
    bool ok = slot.id && filter_ack && accepted;
    const cJSON *item;
    cJSON_ArrayForEach(item, uris)
    {
        if (!ok)
            break;
        const char *uri = cJSON_GetStringValue(item);
        if (!uri) {
            clear_filter(&slot);
            cJSON_Delete(filter_ack);
            cJSON_Delete(accepted);
            return mcp_error(r, id, -32602, "Expected resource URI");
        }
        int index = mcp_resource_index(uri);
        if (index >= 0) {
            if (slot.interests & (1u << index))
                continue;
            slot.interests |= 1u << index;
        } else {
            fabric_property_path_t path;
            if (!mcp_property_path(uri, &path))
                continue;
            bool duplicate = false;
            for (unsigned j = 0; j < slot.count; j++)
                if (!strcmp(slot.exact[j].uri, uri))
                    duplicate = true;
            if (duplicate)
                continue;
            if (slot.count >= MAX_EXACT)
                continue;
            cJSON *prop = NULL;
            esp_err_t err = mcp_get_property(&path, &prop);
            if (err == ESP_ERR_NOT_FOUND)
                continue;
            if (err != ESP_OK) {
                ok = false;
                break;
            }
            if (!slot.exact)
                slot.exact = calloc(MAX_EXACT, sizeof(exact_t));
            if (!slot.exact) {
                cJSON_Delete(prop);
                ok = false;
                break;
            }
            exact_t *e = &slot.exact[slot.count++];
            *e = (exact_t){.path = path, .uri = strdup(uri), .previous = prop};
            if (!e->uri) {
                ok = false;
                break;
            }
            slot.devices |= 1u << (path.device % 32);
        }
        ok = cJSON_AddItemToArray(accepted, cJSON_CreateString(uri));
    }
    if (ok && cJSON_GetArraySize(accepted)) {
        ok = cJSON_AddItemToObject(filter_ack, "resourceSubscriptions", accepted);
        if (ok)
            accepted = NULL;
    }
    char *filter_text = ok ? cJSON_PrintUnformatted(filter_ack) : NULL;
    if (filter_text)
        asprintf(&slot.ack, "\"notifications\":%s", filter_text);
    cJSON_free(filter_text);
    cJSON_Delete(filter_ack);
    cJSON_Delete(accepted);
    slot.changed_devices = slot.devices;
    if (!slot.ack || httpd_req_async_handler_begin(r, &slot.req) != ESP_OK) {
        clear_filter(&slot);
        return mcp_unavailable(r);
    }
    portENTER_CRITICAL(&lock);
    *s = slot;
    portEXIT_CRITICAL(&lock);
    if (xTaskCreate(stream_loop, "mcp_listen", 4096, s, 4, NULL) != pdPASS) {
        mcp_unavailable(slot.req);
        release_stream(s);
    }
    return ESP_OK;
}
