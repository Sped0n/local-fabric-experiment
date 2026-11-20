#pragma once
#include "cJSON.h"
#include "fabric_automation.h"

bool automation_hooks_valid(const fabric_automation_hook_t *hooks, size_t count);
const fabric_automation_hook_t *automation_hook_find(const fabric_automation_hook_t *hooks, size_t count,
                                                     const char *name);
bool automation_hook_args_valid(const fabric_automation_hook_t *action, const cJSON *args);
esp_err_t automation_hooks_json(const fabric_automation_hook_t *hooks, size_t count, char **out);
