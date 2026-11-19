#pragma once
#include "cJSON.h"
#include "lua.h"
#include <stdbool.h>

// Called inside a protected Lua call. Leaves generated source on the stack.
// Input selectors, comparison operators and operand types have already been validated.
void rule_source_push(lua_State *L, const cJSON *definition);
