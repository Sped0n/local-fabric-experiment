#include "console.h"
#include "cJSON.h"
#include "esp_console.h"
#include "esp_http_client.h"
#include <stdio.h>
#include <string.h>

#include "fabric_http.h"
#define VERSION "2026-07-28"
static int request_id;

static esp_err_t response(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA)
        fwrite(event->data, 1, event->data_len, stdout);
    return ESP_OK;
}
static esp_err_t post(const char *body, const char *method, const char *name)
{
    esp_http_client_config_t cfg = {.host = "127.0.0.1",
                                    .port = 80,
                                    .path = "/mcp",
                                    .method = HTTP_METHOD_POST,
                                    .timeout_ms = 5000,
                                    .disable_auto_redirect = true,
                                    .event_handler = response};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
        return ESP_ERR_NO_MEM;
    char authorization[FABRIC_HTTP_AUTHORIZATION_SIZE];
    fabric_http_copy_authorization(authorization);
    esp_err_t err = esp_http_client_set_header(client, "Authorization", authorization);
    if (err == ESP_OK)
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK)
        err = esp_http_client_set_header(client, "Accept", "application/json, text/event-stream");
    if (err == ESP_OK)
        err = esp_http_client_set_header(client, "MCP-Protocol-Version", VERSION);
    if (err == ESP_OK)
        err = esp_http_client_set_header(client, "Mcp-Method", method);
    if (err == ESP_OK && name)
        err = esp_http_client_set_header(client, "Mcp-Name", name);
    if (err == ESP_OK)
        err = esp_http_client_set_post_field(client, body, strlen(body));
    if (err == ESP_OK)
        err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    printf("\nHTTP %d (%s)\n", code, esp_err_to_name(err));
    return err == ESP_OK && code >= 200 && code < 300 ? ESP_OK : ESP_FAIL;
}
static int fabric(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "token")) {
        char header[FABRIC_HTTP_AUTHORIZATION_SIZE];
        fabric_http_copy_authorization(header);
        printf("Authorization: %s\n", header);
        return 0;
    }
    if (argc < 2 || argc > 3) {
        puts("fabric resources/list | fabric resources/read {\"uri\":\"fabric://status\"} | fabric tools/call "
             "{\"name\":...,\"arguments\":{...}}");
        return 1;
    }
    cJSON *params = cJSON_ParseWithOpts(argc == 3 ? argv[2] : "{}", NULL, true);
    if (!cJSON_IsObject(params)) {
        cJSON_Delete(params);
        puts("expected JSON object");
        return 1;
    }
    const char *name = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(params, !strcmp(argv[1], "resources/read") ? "uri" : "name"));
    cJSON *meta = cJSON_Parse("{\"io.modelcontextprotocol/protocolVersion\":\"" VERSION
                              "\",\"io.modelcontextprotocol/clientCapabilities\":{},\"io.modelcontextprotocol/"
                              "clientInfo\":{\"name\":\"uart\",\"version\":\"1\"}}");
    if (!meta || !cJSON_AddItemToObject(params, "_meta", meta)) {
        cJSON_Delete(params);
        cJSON_Delete(meta);
        return 1;
    }
    cJSON *msg = cJSON_CreateObject();
    char *body = NULL;
    if (msg && cJSON_AddItemToObject(msg, "params", params)) {
        params = NULL;
        if (cJSON_AddStringToObject(msg, "jsonrpc", "2.0") && cJSON_AddNumberToObject(msg, "id", ++request_id) &&
            cJSON_AddStringToObject(msg, "method", argv[1]))
            body = cJSON_PrintUnformatted(msg);
    }
    esp_err_t err = body ? post(body, argv[1], name) : ESP_ERR_NO_MEM;
    cJSON_Delete(params);
    cJSON_Delete(msg);
    cJSON_free(body);
    return err != ESP_OK;
}
esp_err_t demo_console_start(void)
{
    esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    config.prompt = "fabric>";
    config.max_cmdline_length = 3072;
    config.task_stack_size = 8192;
    esp_console_repl_t *repl;
    esp_err_t err = esp_console_new_repl_stdio(&config, &repl);
    if (err != ESP_OK)
        return err;
    const esp_console_cmd_t cmd = {
        .command = "fabric", .help = "MCP method [JSON params] or token (show board credential)", .func = fabric};
    err = esp_console_cmd_register(&cmd);
    return err == ESP_OK ? esp_console_start_repl(repl) : err;
}
