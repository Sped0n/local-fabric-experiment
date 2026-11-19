#pragma once
#include "fabric_automation.h"

#define RULE_MAX_STEPS 8
#define RULE_MAX_DELAY_SECONDS 86400

typedef struct action_step {
    struct action_step *next;
    unsigned delay_seconds;
    fabric_manager_device_id_t device;
    uint16_t endpoint;
    const fabric_rule_action_t *native;
    char cluster[48], name[48];
    char *args;
} action_step_t;

void action_steps_free(action_step_t *steps);
typedef struct action_runner action_runner_t;
typedef void (*action_done_fn)(void *ctx, unsigned slot, esp_err_t error);
action_runner_t *action_runner_create(fabric_manager_handle_t manager, action_done_fn done, void *ctx);
void action_runner_delete(action_runner_t *runner);
// Serialized by the automation owner. Cancelling cannot recall an already submitted Matter command.
bool action_runner_active(action_runner_t *runner, unsigned slot);
void action_runner_start(action_runner_t *runner, unsigned slot, action_step_t *steps);
void action_runner_cancel(action_runner_t *runner, unsigned slot);
// Called on the worker task. Returns the earliest remaining monotonic deadline, or INT64_MAX.
int64_t action_runner_advance(action_runner_t *runner);
