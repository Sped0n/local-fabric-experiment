#include "mcp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static fabric_manager_handle_t manager;
static fabric_automation_t automation;
extern const char rule_schema[] asm("_binary_rule_definition_json_start");
const char *const mcp_resource_uris[MCP_RESOURCE_COUNT] = {
    [MCP_STATUS] = "fabric://status",          [MCP_METADATA] = "fabric://devices/metadata",
    [MCP_STATE] = "fabric://devices/state",    [MCP_ACTIVITY] = "fabric://activity",
    [MCP_RULES] = "fabric://automation/rules", [MCP_ACTIONS] = "fabric://automation/actions",
    [MCP_RUNS] = "fabric://automation/runs"};
int mcp_resource_index(const char *uri)
{
    for (int i = 0; uri && i < (automation ? MCP_RESOURCE_COUNT : MCP_RULES); i++)
        if (!strcmp(uri, mcp_resource_uris[i]))
            return i;
    return -1;
}
static bool uri_id(const char **cursor, uint32_t max, uint32_t *out)
{
    const char *p = *cursor;
    uint32_t n = 0;
    if (*p < '0' || *p > '9' || (*p == '0' && p[1] != '/'))
        return false;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = *p++ - '0';
        if (n > (max - digit) / 10)
            return false;
        n = n * 10 + digit;
    }
    *out = n;
    *cursor = p;
    return true;
}
bool mcp_property_path(const char *uri, fabric_property_path_t *p)
{
    const char prefix[] = "fabric://devices/";
    if (!uri || strncmp(uri, prefix, sizeof(prefix) - 1))
        return false;
    uri += sizeof(prefix) - 1;
    uint32_t ep;
    if (!uri_id(&uri, UINT32_MAX, &p->device) || !p->device || strncmp(uri, "/endpoints/", 11))
        return false;
    uri += 11;
    if (!uri_id(&uri, UINT16_MAX, &ep) || strncmp(uri, "/clusters/", 10))
        return false;
    p->endpoint = ep;
    uri += 10;
    const char *letters = "abcdefghijklmnopqrstuvwxyz0123456789_";
    size_t n = strspn(uri, letters);
    if (!n || n >= sizeof(p->cluster) || strncmp(uri + n, "/properties/", 12))
        return false;
    memcpy(p->cluster, uri, n);
    p->cluster[n] = 0;
    uri += n + 12;
    n = strspn(uri, letters);
    if (!n || n >= sizeof(p->property) || uri[n])
        return false;
    strcpy(p->property, uri);
    return true;
}
esp_err_t mcp_get_property(const fabric_property_path_t *path, cJSON **prop)
{
    return fabric_manager_property(manager, path, prop, NULL);
}
void mcp_catalog_init(fabric_manager_handle_t m, fabric_automation_t a)
{
    manager = m;
    automation = a;
}
typedef struct {
    const char *name, *type;
} arg_t;
typedef struct {
    const char *name, *description, *route;
    arg_t args[6];
    bool rule;
} tool_t;
static const tool_t tools[] = {
    {"device.get", "Read a logical device and its capabilities", NULL, {{"device_id", "integer"}}},
    {"device.set_identity",
     "Set the device name and room without changing its stable ID; empty room clears it",
     NULL,
     {{"device_id", "integer"}, {"name", "string"}, {"room", "string"}}},
    {"device.action",
     "Submit an advertised action; observe reports for the outcome",
     "/action",
     {{"device_id", "integer"},
      {"endpoint_id", "integer"},
      {"cluster", "string"},
      {"action", "string"},
      {"arguments", "object"}}},
    {"device.remove", "Remove the whole Matter node, including sibling devices", "/remove", {{"device_id", "integer"}}},
    {"commission.start", "Start on-network commissioning", "/commission", {{"code", "string"}}},
    {"commission.cancel", "Cancel active commissioning", "/cancel", {{0}}},
    {"rule.save", "Validate and commit a rule", NULL, {{"slot", "integer"}, {"definition", "object"}}, true},
    {"rule.remove", "Remove a saved rule", NULL, {{"slot", "integer"}}, true}};
static bool describe(cJSON *list, const tool_t *tool)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return false;
    cJSON_AddItemToArray(list, obj);
    cJSON *schema = cJSON_AddObjectToObject(obj, "inputSchema"), *props = cJSON_AddObjectToObject(schema, "properties"),
          *req = cJSON_AddArrayToObject(schema, "required");
    if (!req || !props || !cJSON_AddStringToObject(obj, "name", tool->name) ||
        !cJSON_AddStringToObject(obj, "description", tool->description) ||
        !cJSON_AddStringToObject(schema, "type", "object") || !cJSON_AddFalseToObject(schema, "additionalProperties"))
        return false;
    for (const arg_t *a = tool->args; a->name; a++) {
        if (!strcmp(a->name, "definition")) {
            cJSON *definition = cJSON_Parse(rule_schema);
            if (!definition || !cJSON_AddItemToObject(props, a->name, definition)) {
                cJSON_Delete(definition);
                return false;
            }
            if (!cJSON_AddItemToArray(req, cJSON_CreateString(a->name)))
                return false;
            continue;
        }
        cJSON *p = cJSON_AddObjectToObject(props, a->name);
        if (!strcmp(a->name, "slot") &&
            (!cJSON_AddNumberToObject(p, "minimum", 1) ||
             !cJSON_AddNumberToObject(p, "maximum", FABRIC_AUTOMATION_SLOTS)))
            return false;
        if (!p || !cJSON_AddStringToObject(p, "type", a->type) ||
            !cJSON_AddItemToArray(req, cJSON_CreateString(a->name)))
            return false;
    }
    return true;
}
static esp_err_t list_items(httpd_req_t *r, const char *id, bool list_tools)
{
    cJSON *root = cJSON_Parse("{\"resultType\":\"complete\"," MCP_CACHE "}"),
          *list = cJSON_AddArrayToObject(root, list_tools ? "tools" : "resources");
    bool ok = list != NULL;
    if (list_tools) {
        for (size_t i = 0; ok && i < sizeof(tools) / sizeof(*tools); i++)
            if (automation || !tools[i].rule)
                ok = describe(list, &tools[i]);
    } else
        for (int i = 0; ok && i < (automation ? MCP_RESOURCE_COUNT : MCP_RULES); i++) {
            cJSON *obj = cJSON_CreateObject();
            ok = cJSON_AddItemToArray(list, obj) && cJSON_AddStringToObject(obj, "uri", mcp_resource_uris[i]) &&
                cJSON_AddStringToObject(obj, "name", mcp_resource_uris[i] + 9) &&
                cJSON_AddStringToObject(obj, "mimeType", "application/json");
        }
    char *out = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    esp_err_t err = mcp_result(r, id, out);
    cJSON_free(out);
    return err;
}
static esp_err_t read_resource(httpd_req_t *r, const char *id, const char *uri)
{
    int index = mcp_resource_index(uri);
    char *json = NULL;
#if CONFIG_FABRIC_AUTOMATION
    const char prefix[] = "fabric://automation/runs/";
    if (automation && uri && !strncmp(uri, prefix, sizeof(prefix) - 1)) {
        const char *cursor = uri + sizeof(prefix) - 1;
        uint32_t run = 0;
        if (!uri_id(&cursor, UINT32_MAX, &run) || *cursor || !run)
            return mcp_error(r, id, -32002, "Unknown run");
        esp_err_t err = fabric_automation_runs(automation, run, &json);
        if (err == ESP_ERR_NOT_FOUND)
            return mcp_error(r, id, -32002, "Run no longer available");
    } else
#endif
        if (index < 0) {
        fabric_property_path_t p;
        cJSON *prop = NULL;
        if (!mcp_property_path(uri, &p))
            return mcp_error(r, id, -32002, "Unknown resource");
        esp_err_t err = mcp_get_property(&p, &prop);
        if (err == ESP_ERR_NOT_FOUND)
            return mcp_error(r, id, -32002, "Unknown property");
        if (err == ESP_OK)
            json = cJSON_PrintUnformatted(prop);
        cJSON_Delete(prop);
    }
    if (index == MCP_STATUS)
        json = http_status_json();
    if (index == MCP_ACTIVITY)
        fabric_manager_activity_json(manager, &json);
    if (index == MCP_METADATA || index == MCP_STATE)
        fabric_manager_devices_json(manager, index == MCP_METADATA ? FABRIC_DEVICE_METADATA : FABRIC_DEVICE_STATE,
                                    &json);
#if CONFIG_FABRIC_AUTOMATION
    if (index == MCP_RULES)
        fabric_automation_list(automation, &json);
    if (index == MCP_ACTIONS)
        fabric_automation_list_actions(automation, &json);
    if (index == MCP_RUNS)
        fabric_automation_runs(automation, 0, &json);
#endif
    if (!json)
        return mcp_unavailable(r);
    cJSON *text = cJSON_CreateStringReference(json);
    char *quoted = text ? cJSON_PrintUnformatted(text) : NULL, *result = NULL;
    cJSON_Delete(text);
    free(json);
    if (quoted)
        asprintf(&result,
                 "{\"resultType\":\"complete\"," MCP_CACHE
                 ",\"contents\":[{\"uri\":\"%s\",\"mimeType\":\"application/json\",\"text\":%s}]}",
                 uri, quoted);
    cJSON_free(quoted);
    esp_err_t err = mcp_result(r, id, result);
    free(result);
    return err;
}
static bool valid_args(const tool_t *tool, const cJSON *args)
{
    if (args && !cJSON_IsObject(args))
        return false;
    int count = 0;
    for (const arg_t *a = tool->args; a->name; a++) {
        count++;
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, a->name);
        if (!strcmp(a->type, "string")) {
            if (!cJSON_IsString(v))
                return false;
        } else if (!strcmp(a->type, "object")) {
            if (!cJSON_IsObject(v))
                return false;
        } else if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= UINT32_MAX) ||
                   trunc(v->valuedouble) != v->valuedouble)
            return false;
    }
    return count == cJSON_GetArraySize(args);
}
static esp_err_t call_tool(httpd_req_t *r, const char *id, const char *name, const cJSON *args)
{
    const tool_t *tool = NULL;
    for (size_t i = 0; i < sizeof(tools) / sizeof(*tools); i++)
        if ((automation || !tools[i].rule) && !strcmp(name, tools[i].name)) {
            tool = &tools[i];
            break;
        }
    if (!tool)
        return mcp_error(r, id, -32602, "Unknown tool");
    if (!valid_args(tool, args))
        return mcp_error(r, id, -32602, "Invalid tool arguments");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    char *json = NULL, diagnostic[128] = {0};
    if (tool->route)
        err = http_submit_control(tool->route, args);
    else if (!strcmp(name, "device.set_identity"))
        err = fabric_manager_set_device_identity(
            manager, (uint32_t)cJSON_GetObjectItemCaseSensitive(args, "device_id")->valuedouble,
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "name")),
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "room")));
    else if (!strcmp(name, "device.get"))
        err = fabric_manager_get_device_json(
            manager, (uint32_t)cJSON_GetObjectItemCaseSensitive(args, "device_id")->valuedouble, &json);
#if CONFIG_FABRIC_AUTOMATION
    else {
        int slot = cJSON_GetObjectItemCaseSensitive(args, "slot")->valueint;
        if (slot >= 1 && slot <= FABRIC_AUTOMATION_SLOTS) {
            if (!strcmp(name, "rule.remove"))
                err = fabric_automation_save(automation, slot, NULL, diagnostic, sizeof(diagnostic));
            else {
                char *definition = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(args, "definition"));
                err = definition ? fabric_automation_save(automation, slot, definition, diagnostic, sizeof(diagnostic))
                                 : ESP_ERR_NO_MEM;
                cJSON_free(definition);
            }
        }
    }
#endif
    cJSON *text = cJSON_CreateStringReference(
        err == ESP_OK       ? (json                                                       ? json
                                   : (tool->rule || !strcmp(name, "device.set_identity")) ? "committed"
                                                                                          : "accepted")
            : diagnostic[0] ? diagnostic
                            : esp_err_to_name(err));
    char *quoted = text ? cJSON_PrintUnformatted(text) : NULL, *result = NULL;
    cJSON_Delete(text);
    free(json);
    if (quoted)
        asprintf(&result, "{\"resultType\":\"complete\",\"isError\":%s,\"content\":[{\"type\":\"text\",\"text\":%s}]}",
                 err == ESP_OK ? "false" : "true", quoted);
    cJSON_free(quoted);
    err = mcp_result(r, id, result);
    free(result);
    return err;
}
esp_err_t mcp_catalog_request(httpd_req_t *r, const char *id, const char *method, const cJSON *params)
{
    if (!strcmp(method, "server/discover"))
        return mcp_result(
            r, id,
            "{\"resultType\":\"complete\"," MCP_CACHE ",\"supportedVersions\":[\"" MCP_VERSION
            "\"],\"capabilities\":{\"tools\":{},\"resources\":{\"subscribe\":true}},\"_meta\":{\"" MCP_META
            "serverInfo\":{\"name\":\"local-fabric\",\"version\":\"1\"}}}");
    if (!strcmp(method, "ping"))
        return mcp_result(r, id, MCP_COMPLETE);
    if (!strcmp(method, "resources/templates/list")) {
        const char *prop = "{\"name\":\"device "
                           "property\",\"uriTemplate\":\"fabric://devices/{device}/endpoints/{endpoint}/clusters/"
                           "{cluster}/properties/{property}\",\"mimeType\":\"application/json\"}";
        char body[512];
        snprintf(body, sizeof(body), "{\"resultType\":\"complete\"," MCP_CACHE ",\"resourceTemplates\":[%s%s]}", prop,
                 automation
                     ? ",{\"name\":\"automation "
                       "run\",\"uriTemplate\":\"fabric://automation/runs/{id}\",\"mimeType\":\"application/json\"}"
                     : "");
        return mcp_result(r, id, body);
    }
    if (!strcmp(method, "tools/list") || !strcmp(method, "resources/list"))
        return list_items(r, id, !strcmp(method, "tools/list"));
    if (!strcmp(method, "resources/read"))
        return read_resource(r, id, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "uri")));
    if (!strcmp(method, "tools/call"))
        return call_tool(r, id, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "name")),
                         cJSON_GetObjectItemCaseSensitive(params, "arguments"));
    return mcp_error(r, id, -32601, "Method not found");
}
