#pragma once
#include "fabric_manager.h"
#ifdef __cplusplus
extern "C" {
#endif
// One owner serializes configuration calls. Rule tasks never hold CHIP locks.
typedef struct fabric_automation *fabric_automation_t;
/** Runs on the shared worker. Copy any retained arguments before returning.
 * Return ESP_OK on acceptance, or an error (e.g. ESP_ERR_TIMEOUT if full).
 * Never wait for queue capacity, I/O or agent completion. */
typedef esp_err_t (*fabric_rule_enqueue_fn)(void *ctx, const char *arguments_json);
typedef enum { FABRIC_ARG_STRING, FABRIC_ARG_BOOLEAN, FABRIC_ARG_INTEGER, FABRIC_ARG_NUMBER } fabric_rule_arg_type_t;
typedef struct {
    const char *name;
    fabric_rule_arg_type_t type;
} fabric_rule_arg_t;
typedef struct {
    const char *name, *description;
    const fabric_rule_arg_t *arguments; // Exact required scalar fields; NULL for no arguments.
    size_t argument_count;
    fabric_rule_enqueue_fn try_enqueue;
    void *context;
} fabric_rule_action_t;
// The registration table, metadata and contexts remain valid for the lifetime of the service.
esp_err_t fabric_automation_init(fabric_manager_handle_t manager, const fabric_rule_action_t *actions, size_t count,
                                 fabric_automation_t *out);
#define FABRIC_AUTOMATION_SLOTS 32
// Stable rule slots 1..32. Save validates before replacing; NULL removes a slot.
esp_err_t fabric_automation_save(fabric_automation_t service, unsigned slot, const char *definition, char *diagnostic,
                                 size_t size);
esp_err_t fabric_automation_list(fabric_automation_t service, char **owned_json);
/** Immutable local action catalog (name, description and arguments); caller frees the JSON with free(). */
esp_err_t fabric_automation_list_actions(fabric_automation_t service, char **owned_json);
// After system wall time is synchronized, allow local_time() in subsequent handlers.
void fabric_automation_clock_synchronized(fabric_automation_t service);
/** Optional nonblocking invalidation callback, installed by the configuration owner. */
void fabric_automation_set_changed_handler(fabric_automation_t service, void (*changed)(void));
void fabric_automation_close(fabric_automation_t service);
#ifdef __cplusplus
}
#endif
