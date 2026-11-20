#include "esp_timer.h"
#include "fabric_manager_private.h"
#include <cstdlib>
#include <cstring>

// Called under the manager lock. Only normalized public scalars reach the ring.
void device_activity_append(device_activity_t *activity, uint32_t device, uint16_t endpoint, const char *cluster,
                            const cJSON *property)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(property, "value");
    auto &row = activity->entries[activity->next];
    row = {};
    row.uptime_ms = esp_timer_get_time() / 1000;
    row.wall = time(nullptr);
    row.cluster = cluster;
    row.device = device;
    row.endpoint = endpoint;
    strlcpy(row.property, property->string, sizeof(row.property));
    const char *unit = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(property, "unit"));
    if (unit)
        strlcpy(row.unit, unit, sizeof(row.unit));
    row.type = value->type & 0xff;
    if (cJSON_IsNumber(value))
        row.value.number = value->valuedouble;
    if (cJSON_IsString(value)) {
        const char *s = value->valuestring;
        size_t n = strlcpy(row.value.text, s, sizeof(row.value.text));
        if (n >= sizeof(row.value.text)) {
            row.truncated = true;
            size_t end = sizeof(row.value.text) - 1;
            while (end && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80) --end;
            row.value.text[end] = 0;
        }
    }
    activity->next = (activity->next + 1) % DEVICE_ACTIVITY_CAPACITY;
    if (activity->count < DEVICE_ACTIVITY_CAPACITY)
        ++activity->count;
}

static bool add_value(cJSON *row, const device_activity_entry_t &e)
{
    switch (e.type) {
    case cJSON_Number:
        return cJSON_AddNumberToObject(row, "current", e.value.number);
    case cJSON_String:
        return cJSON_AddStringToObject(row, "current", e.value.text);
    case cJSON_True:
    case cJSON_False:
        return cJSON_AddBoolToObject(row, "current", e.type == cJSON_True);
    default:
        return cJSON_AddNullToObject(row, "current");
    }
}
esp_err_t fabric_manager_activity_json(fabric_manager_handle_t manager, char **out)
{
    if (!manager || !out)
        return ESP_ERR_INVALID_ARG;
    *out = nullptr;
    auto *snapshot = static_cast<device_activity_t *>(malloc(sizeof(device_activity_t)));
    if (!snapshot)
        return ESP_ERR_NO_MEM;
    {
        data_model::lock_guard lock(manager->state_mutex);
        *snapshot = manager->activity;
    }
    cJSON *rows = cJSON_CreateArray();
    bool ok = rows != nullptr;
    for (unsigned i = 0; ok && i < snapshot->count; ++i) {
        const auto &e =
            snapshot->entries[(snapshot->next + DEVICE_ACTIVITY_CAPACITY - 1 - i) % DEVICE_ACTIVITY_CAPACITY];
        cJSON *row = cJSON_CreateObject();
        ok = row && cJSON_AddItemToArray(rows, row) && cJSON_AddNumberToObject(row, "device", e.device) &&
            cJSON_AddNumberToObject(row, "endpoint", e.endpoint) &&
            cJSON_AddStringToObject(row, "cluster", e.cluster) &&
            cJSON_AddStringToObject(row, "property", e.property) && add_value(row, e) &&
            (!e.unit[0] || cJSON_AddStringToObject(row, "unit", e.unit)) &&
            (!e.truncated || cJSON_AddTrueToObject(row, "truncated")) &&
            cJSON_AddNumberToObject(row, "uptime_ms", e.uptime_ms) &&
            (e.wall < 1577836800 || cJSON_AddNumberToObject(row, "time", e.wall));
    }
    free(snapshot);
    if (ok)
        *out = cJSON_PrintUnformatted(rows);
    cJSON_Delete(rows);
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}
