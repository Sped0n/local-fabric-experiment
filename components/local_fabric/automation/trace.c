#include "trace.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "fabric_utf8.h"
#include <stdio.h>
#include <string.h>

#define CAPACITY CONFIG_FABRIC_AUTOMATION_ACTIVITY_CAPACITY
void automation_trace_append(automation_trace_ring_t *h, const automation_trace_start_t *start, uint32_t rule_id,
                             unsigned slot, const char *title, const char *outcome, const char *reason)
{
    automation_trace_record_t *r = &h->finished[h->next];
    r->start = *start;
    r->rule_id = rule_id;
    r->slot = slot + 1;
    r->elapsed_ms = (esp_timer_get_time() - start->start_us) / 1000;
    r->outcome = outcome;
    fabric_utf8_copy(r->title, sizeof(r->title), title);
    fabric_utf8_copy(r->reason, sizeof(r->reason), reason);
    h->next = (h->next + 1) % CAPACITY;
    if (h->count < CAPACITY)
        ++h->count;
}
static bool record_json(cJSON *row, const automation_trace_record_t *r)
{
    return cJSON_AddNumberToObject(row, "id", r->start.id) && cJSON_AddNumberToObject(row, "rule_id", r->rule_id) &&
        cJSON_AddNumberToObject(row, "slot", r->slot) && cJSON_AddStringToObject(row, "title", r->title) &&
        cJSON_AddNumberToObject(row, "uptime_ms", r->start.start_us / 1000) &&
        (!r->start.wall || cJSON_AddNumberToObject(row, "time", r->start.wall)) &&
        cJSON_AddNumberToObject(row, "elapsed_ms", r->elapsed_ms) &&
        cJSON_AddStringToObject(row, "outcome", r->outcome) &&
        cJSON_AddStringToObject(row, "trigger", r->start.trigger) &&
        (!r->reason[0] || cJSON_AddStringToObject(row, "reason", r->reason));
}
esp_err_t automation_trace_json(const automation_trace_ring_t *h, uint32_t id, char **out)
{
    *out = NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return ESP_ERR_NO_MEM;
    cJSON *rows = id ? NULL : cJSON_AddArrayToObject(root, "finished");
    bool ok = id || (rows && cJSON_AddNumberToObject(root, "capacity", CAPACITY));
    bool found = !id;
    for (unsigned i = 0; ok && i < h->count; i++) {
        const automation_trace_record_t *r = &h->finished[(h->next + CAPACITY - 1 - i) % CAPACITY];
        if (id && r->start.id != id)
            continue;
        found = true;
        cJSON *row = id ? root : cJSON_CreateObject();
        ok = row && (id || cJSON_AddItemToArray(rows, row)) && record_json(row, r);
        if (id)
            break;
    }
    if (ok && found)
        *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out ? ESP_OK : (!found ? ESP_ERR_NOT_FOUND : ESP_ERR_NO_MEM);
}
