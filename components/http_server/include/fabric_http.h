#pragma once
#include "fabric_automation.h"

#define FABRIC_HTTP_AUTHORIZATION_SIZE 40 // "Bearer " + 32 hex digits + NUL
esp_err_t fabric_http_start(fabric_core_handle_t core, fabric_manager_handle_t manager, fabric_automation_t automation);
// Trusted local console only. Never return the current token from a network getter.
void fabric_http_copy_authorization(char out[FABRIC_HTTP_AUTHORIZATION_SIZE]);
