#pragma once
#include "cJSON.h"
#include "esp_http_server.h"
#include "fabric_automation.h"
esp_err_t fabric_control_submit(const char *route, const cJSON *obj);
char *fabric_status_json(void);
void fabric_http_status_changed(void);
esp_err_t fabric_http_mcp_start(httpd_handle_t server, fabric_manager_handle_t manager, fabric_automation_t automation);
esp_err_t fabric_http_access_init(void);
esp_err_t fabric_http_settings_start(httpd_handle_t server);
void fabric_http_revoke_streams(void);
bool fabric_http_origin_ok(httpd_req_t *req);
bool fabric_http_authorized(httpd_req_t *req);
esp_err_t fabric_http_web_start(httpd_handle_t server, fabric_manager_handle_t manager);
