#pragma once
#include "web_routes.h"
#define MCP_VERSION "2026-07-28"
#define MCP_META "io.modelcontextprotocol/"
#define MCP_COMPLETE "{\"resultType\":\"complete\"}"
#define MCP_CACHE "\"ttlMs\":0,\"cacheScope\":\"private\""
enum { MCP_STATUS, MCP_METADATA, MCP_STATE, MCP_RULES, MCP_ACTIONS, MCP_RESOURCE_COUNT };
extern const char *const mcp_resource_uris[MCP_RESOURCE_COUNT];
bool mcp_property_path(const char *uri, fabric_property_path_t *path);
esp_err_t mcp_get_property(const fabric_property_path_t *path, cJSON **prop);
int mcp_resource_index(const char *uri);
esp_err_t mcp_catalog_request(httpd_req_t *req, const char *id, const char *method, const cJSON *params);
void mcp_catalog_init(fabric_manager_handle_t manager, fabric_automation_t automation);
esp_err_t mcp_watch_start(fabric_manager_handle_t manager, fabric_automation_t automation);
esp_err_t mcp_listen(httpd_req_t *req, const char *id, const cJSON *params);
esp_err_t mcp_result(httpd_req_t *req, const char *id, const char *result);
esp_err_t mcp_error(httpd_req_t *req, const char *id, int code, const char *message);
esp_err_t mcp_unavailable(httpd_req_t *req);
