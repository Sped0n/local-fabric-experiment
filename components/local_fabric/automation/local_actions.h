#pragma once
#include "cJSON.h"
#include "fabric_automation.h"

bool local_actions_valid(const fabric_rule_action_t *actions, size_t count);
const fabric_rule_action_t *local_action_find(const fabric_rule_action_t *actions, size_t count, const char *name);
bool local_action_accepts(const fabric_rule_action_t *action, const cJSON *args);
esp_err_t local_actions_json(const fabric_rule_action_t *actions, size_t count, char **out);
