#pragma once
#include "esp_err.h"
#include "sdkconfig.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Held by the rule until its evaluation or action sequence terminates.
typedef struct {
    int64_t start_us;
    time_t wall;
    uint32_t id;
    char trigger[144];
} automation_trace_start_t;
typedef struct {
    automation_trace_start_t start;
    uint32_t rule_id, slot, elapsed_ms;
    const char *outcome; // Static storage.
    char title[65], reason[160];
} automation_trace_record_t;
typedef struct {
    uint32_t next_id;
    unsigned next, count;
    automation_trace_record_t finished[CONFIG_FABRIC_AUTOMATION_ACTIVITY_CAPACITY];
} automation_trace_ring_t;
// All calls are serialized by the automation worker's owner lock.
void automation_trace_append(automation_trace_ring_t *h, const automation_trace_start_t *start, uint32_t rule_id,
                             unsigned slot, const char *title, const char *outcome, const char *reason);
esp_err_t automation_trace_json(const automation_trace_ring_t *h, uint32_t id, char **out);
