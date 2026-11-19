/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/**
 * @file
 * @brief Resolved Local Fabric devices expressed as endpoints and clusters.
 *
 * Matter topology and node addresses are private to the device manager. Public
 * operations use a stable device id plus an endpoint/cluster/member coordinate.
 */

#include "cJSON.h"
#include "esp_err.h"
#include "fabric_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FABRIC_MANAGER_NAME_MAX 64U
#define FABRIC_MANAGER_ROOM_MAX 64U
#define FABRIC_MANAGER_CLUSTER_KEY_MAX 40U
#define FABRIC_MANAGER_MEMBER_KEY_MAX 40U

/** Opaque runtime handle. */
typedef struct fabric_manager *fabric_manager_handle_t;
/** Stable consumer identity; it intentionally does not encode Matter topology.
 */
typedef uint32_t fabric_manager_device_id_t;
typedef struct {
    uint32_t device;
    uint16_t endpoint;
    char cluster[40], property[40];
} fabric_property_path_t;
typedef enum { FABRIC_DEVICE_METADATA = 1, FABRIC_DEVICE_STATE = 2 } fabric_device_view_t;
// Under the Manager lock: signal only. Views is a bitmask; devices is a hint (IDs modulo 32).
typedef void (*fabric_change_signal_t)(void *ctx, unsigned views, uint32_t devices);
esp_err_t fabric_manager_add_signal(fabric_manager_handle_t manager, fabric_change_signal_t notify, void *ctx);
void fabric_manager_remove_signal(fabric_manager_handle_t manager, fabric_change_signal_t notify, void *ctx);
#define FABRIC_MANAGER_PROPERTY_BATCH_MAX 4
// Capture all cluster states under one lock. Missing paths yield NULL entries.
esp_err_t fabric_manager_properties(fabric_manager_handle_t manager, const fabric_property_path_t *const *paths,
                                    size_t count, cJSON **out, uint32_t *epochs);
// Owned semantic descriptor. Offline values are null. Epoch changes on a new baseline.
esp_err_t fabric_manager_property(fabric_manager_handle_t manager, const fabric_property_path_t *path, cJSON **out,
                                  uint32_t *epoch);
/** Creates and attaches Manager to a stopped Local Fabric Core. */
esp_err_t fabric_manager_create(fabric_core_handle_t core, fabric_manager_handle_t *out_manager);
/**
 * Detaches and destroys Manager after the core has stopped and other API callers
 * have quiesced. Returns ESP_ERR_INVALID_STATE while callbacks or listener
 * removal are in progress, including deletion attempted from a callback.
 */
esp_err_t fabric_manager_delete(fabric_manager_handle_t manager);

/** Allocate one aggregate {"devices":{"<id>":{...}}}. Metadata contains type/name/room; state contains
 * online/endpoints. */
esp_err_t fabric_manager_devices_json(fabric_manager_handle_t manager, fabric_device_view_t view, char **out);
/** Allocate a coherent snapshot of all devices including endpoints. */
esp_err_t fabric_manager_snapshot_json(fabric_manager_handle_t manager, char **out);
/**
 * Allocates the authoritative device-summary document.
 *
 * The result has shape `{ "devices": [...] }` and must be released with
 * `fabric_manager_free_json()`.
 */
esp_err_t fabric_manager_list_devices_json(fabric_manager_handle_t manager, char **out_json);

/**
 * Allocates one endpoint/cluster device-detail document.
 *
 * The result must be released with `fabric_manager_free_json()`.
 */
esp_err_t fabric_manager_get_device_json(fabric_manager_handle_t manager, fabric_manager_device_id_t device_id,
                                         char **out_json);

/** Releases JSON returned by Manager. NULL is accepted. */
void fabric_manager_free_json(char *json);

/** Updates consumer-owned presentation identity and persists it atomically. */
esp_err_t fabric_manager_set_device_identity(fabric_manager_handle_t manager, fabric_manager_device_id_t device_id,
                                             const char *name, const char *room);

/**
 * Invokes one advertised action by explicit coordinate.
 *
 * `arguments_json` must contain exactly the arguments advertised by the action.
 * Successful command completion does not mutate property state; reports remain
 * authoritative.
 */
esp_err_t fabric_manager_invoke_action(fabric_manager_handle_t manager, fabric_manager_device_id_t device_id,
                                       uint16_t endpoint_id, const char *cluster, const char *action,
                                       const char *arguments_json, fabric_core_request_completion_cb_t completion_cb,
                                       void *user_ctx);

/** Removes the Matter node containing the resolved device. */
esp_err_t fabric_manager_remove_device(fabric_manager_handle_t manager, fabric_manager_device_id_t device_id,
                                       bool force, fabric_core_request_completion_cb_t completion_cb, void *user_ctx);

#ifdef __cplusplus
}
#endif
