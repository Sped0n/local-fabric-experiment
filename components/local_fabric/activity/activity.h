#pragma once
#include "cJSON.h"
#include "fabric_manager.h"
#include "sdkconfig.h"
#include <time.h>

#define DEVICE_ACTIVITY_CAPACITY CONFIG_FABRIC_DEVICE_ACTIVITY_CAPACITY
// Only cluster points into a static adapter. No JSON or device storage is borrowed.
typedef struct {
    int64_t uptime_ms;
    time_t wall;
    const char *cluster;
    uint32_t device;
    uint16_t endpoint;
    uint8_t type;
    bool truncated;
    char property[FABRIC_MANAGER_MEMBER_KEY_MAX], unit[12];
    union {
        double number;
        char text[40];
    } value;
} device_activity_entry_t;
typedef struct {
    device_activity_entry_t entries[DEVICE_ACTIVITY_CAPACITY];
    unsigned next, count;
} device_activity_t;
void device_activity_append(device_activity_t *activity, uint32_t device, uint16_t endpoint, const char *cluster,
                            const cJSON *property);
