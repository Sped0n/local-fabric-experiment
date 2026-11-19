#include "property_test.h"
#include <math.h>
#include <string.h>

static bool is(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}
static bool property_test_known(const property_test_t *t, const cJSON *v)
{
    if (is(t->type, "boolean"))
        return cJSON_IsBool(v);
    if (is(t->type, "string"))
        return cJSON_IsString(v);
    if (is(t->type, "enum"))
        return cJSON_IsString(v) ||
            (cJSON_IsNumber(v) && isfinite(v->valuedouble) && trunc(v->valuedouble) == v->valuedouble);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble);
}
bool property_test_parse(property_test_t *out, const cJSON *spec, bool trigger)
{
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(spec, "type"));
    const char *op = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(spec, "op"));
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(spec, "value");
    if (!is(type, "boolean") && !is(type, "number") && !is(type, "enum") && !is(type, "string"))
        return false;
    static const char *const ops[] = {"true", "false", "==", "!=", "<", "<=", ">", ">=", "changes"};
    for (unsigned i = 0; i < sizeof(ops) / sizeof(*ops); i++)
        if (is(op, ops[i])) {
            *out = (property_test_t){type, i, v};
            if (i == TEST_CHANGES)
                return trigger && !v;
            if (i <= TEST_FALSE)
                return is(type, "boolean") && !v;
            return !is(type, "boolean") && (i <= TEST_NE || is(type, "number")) && property_test_known(out, v);
        }
    return false;
}
bool property_test_supported(const property_test_t *t, const cJSON *p)
{
    if (!is(t->type, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "type"))))
        return false;
    if (!t->value)
        return true;
    if (is(t->type, "number")) {
        double v = t->value->valuedouble, lo = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "min")),
               hi = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "max")),
               step = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "step"));
        return v >= lo && v <= hi && step > 0 && fabs(v / step - round(v / step)) < 1e-7;
    }
    if (is(t->type, "enum")) {
        const cJSON *v;
        cJSON_ArrayForEach(
            v,
            cJSON_GetObjectItemCaseSensitive(
                p, "enum")) if (cJSON_Compare(t->value,
                                              cJSON_IsObject(v) ? cJSON_GetObjectItemCaseSensitive(v, "value") : v,
                                              true)) return true;
        // Some devices expose only the current mode, not a list of possible modes.
        return cJSON_Compare(t->value, cJSON_GetObjectItemCaseSensitive(p, "value"), true);
    }
    return true;
}
