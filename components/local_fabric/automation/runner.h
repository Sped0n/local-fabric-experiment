#pragma once
#include "fabric_automation.h"

#define RULE_MAX_STEPS 8
#define RULE_MAX_DELAY_SECONDS 86400

typedef struct automation_step {
    struct automation_step *next;
    unsigned delay_seconds;
    fabric_manager_device_id_t device;
    uint16_t endpoint;
    const fabric_automation_hook_t *hook;
    char cluster[48], name[48];
    char *args;
} automation_step_t;

void automation_steps_free(automation_step_t *steps);
typedef struct automation_runner automation_runner_t;
// On failure the current step is borrowed only during the callback.
typedef void (*automation_runner_done_fn)(void *ctx, unsigned slot, esp_err_t error, unsigned position,
                                          const automation_step_t *failed);
automation_runner_t *automation_runner_create(fabric_manager_handle_t manager, automation_runner_done_fn done,
                                              void *ctx);
void automation_runner_delete(automation_runner_t *runner);
// Serialized by the automation owner. Cancelling cannot recall an already submitted Matter command.
bool automation_runner_active(automation_runner_t *runner, unsigned slot);
void automation_runner_start(automation_runner_t *runner, unsigned slot, automation_step_t *steps);
void automation_runner_cancel(automation_runner_t *runner, unsigned slot);
// Called on the worker task. Returns the earliest remaining monotonic deadline, or INT64_MAX.
int64_t automation_runner_advance(automation_runner_t *runner);
