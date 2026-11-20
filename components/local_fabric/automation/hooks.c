#include "hooks.h"
#include <math.h>
#include <string.h>

static const char *type_name(fabric_automation_arg_type_t type)
{
    switch (type) {
    case FABRIC_AUTOMATION_ARG_STRING:
        return "string";
    case FABRIC_AUTOMATION_ARG_BOOLEAN:
        return "boolean";
    case FABRIC_AUTOMATION_ARG_INTEGER:
        return "integer";
    case FABRIC_AUTOMATION_ARG_NUMBER:
        return "number";
    default:
        return NULL;
    }
}
const fabric_automation_hook_t *automation_hook_find(const fabric_automation_hook_t *hooks, size_t count,
                                                     const char *name)
{
    for (size_t i = 0; i < count; i++)
        if (!strcmp(name, hooks[i].name))
            return &hooks[i];
    return NULL;
}
bool automation_hooks_valid(const fabric_automation_hook_t *hooks, size_t count)
{
    if (count && !hooks)
        return false;
    for (size_t i = 0; i < count; i++) {
        const fabric_automation_hook_t *a = &hooks[i];
        if (!a->name || !*a->name || strlen(a->name) >= 48 || automation_hook_find(hooks, i, a->name) ||
            !a->description || !*a->description || !a->try_enqueue || a->argument_count > 16 ||
            (a->argument_count && !a->arguments))
            return false;
        for (size_t j = 0; j < a->argument_count; j++) {
            const fabric_automation_arg_t *p = &a->arguments[j];
            if (!p->name || !*p->name || strlen(p->name) >= 64 || !type_name(p->type))
                return false;
            for (size_t k = 0; k < j; k++)
                if (!strcmp(p->name, a->arguments[k].name))
                    return false;
        }
    }
    return true;
}
bool automation_hook_args_valid(const fabric_automation_hook_t *a, const cJSON *args)
{
    if (!a || !cJSON_IsObject(args) || (size_t)cJSON_GetArraySize(args) != a->argument_count)
        return false;
    for (size_t j = 0; j < a->argument_count; j++) {
        const fabric_automation_arg_t *p = &a->arguments[j];
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, p->name);
        switch (p->type) {
        case FABRIC_AUTOMATION_ARG_STRING:
            if (!cJSON_IsString(v) || strlen(v->valuestring) > 512)
                return false;
            break;
        case FABRIC_AUTOMATION_ARG_BOOLEAN:
            if (!cJSON_IsBool(v))
                return false;
            break;
        case FABRIC_AUTOMATION_ARG_INTEGER:
        case FABRIC_AUTOMATION_ARG_NUMBER:
            if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
                (p->type == FABRIC_AUTOMATION_ARG_INTEGER && trunc(v->valuedouble) != v->valuedouble))
                return false;
            break;
        default:
            return false;
        }
    }
    return true;
}
esp_err_t automation_hooks_json(const fabric_automation_hook_t *hooks, size_t count, char **out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    *out = NULL;
    cJSON *list = cJSON_CreateArray();
    if (!list)
        return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < count; i++) {
        const fabric_automation_hook_t *a = &hooks[i];
        cJSON *row = cJSON_CreateObject();
        if (!cJSON_AddItemToArray(list, row)) {
            cJSON_Delete(row);
            goto done;
        }
        if (!cJSON_AddStringToObject(row, "name", a->name) ||
            !cJSON_AddStringToObject(row, "description", a->description))
            goto done;
        cJSON *fields = cJSON_AddObjectToObject(row, "arguments");
        if (!fields)
            goto done;
        for (size_t j = 0; j < a->argument_count; j++) {
            const fabric_automation_arg_t *p = &a->arguments[j];
            cJSON *arg = cJSON_AddObjectToObject(fields, p->name);
            if (!arg || !cJSON_AddStringToObject(arg, "type", type_name(p->type)))
                goto done;
        }
    }
    *out = cJSON_PrintUnformatted(list);
done:
    cJSON_Delete(list);
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}
