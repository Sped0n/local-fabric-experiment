/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/**
 * @file
 * @brief Private Local Fabric Core state and integration seams.
 */

#include "fabric_core.h"

#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <lib/core/CHIPError.h>
#include <lib/core/TLV.h>

/* Keep persisted namespaces stable across public API renames. */
#define FABRIC_CORE_NVS_NAMESPACE "lf_core"
#define FABRIC_CORE_NODE_IDS_KEY "nodes"
#define FABRIC_CORE_IDENTITY_KEY "identity"
#define FABRIC_CORE_MAX_SETUP_CODE_LENGTH 256U

struct fabric_core_subscription;

/** Commissioned node and its owned subscription. */
struct fabric_core_node {
    uint64_t node_id;
    fabric_core_subscription *subscription;
};

/** Callback-scoped raw TLV attribute report for the private semantic sink. */
struct fabric_core_private_attribute_report {
    fabric_core_attribute_path_t path;
    chip::TLV::TLVReader *data;
    CHIP_ERROR error;
};

/** Single model listener used by the optional semantic device manager. */
struct core_model_listener {
    /** Called after the commissioned node set changes. */
    void (*node_set_changed)(fabric_core_handle_t core, void *user_ctx);
    /** Called when the subscription is lost or stopped. A committed snapshot brings the node online. */
    void (*node_offline)(fabric_core_handle_t core, uint64_t node_id, void *user_ctx);
    /** Opens one report transaction for the given node. */
    void (*report_begin)(fabric_core_handle_t core, uint64_t node_id, bool priming, void *user_ctx);
    /** Delivers one raw TLV attribute in the corresponding node report. */
    void (*attribute_reported)(fabric_core_handle_t core, const fabric_core_private_attribute_report *report,
                               void *user_ctx);
    /**
     * Closes the active report transaction. snapshot_complete is true for the
     * priming wildcard report after a subscription starts or resubscribes.
     */
    void (*report_end)(fabric_core_handle_t core, uint64_t node_id, esp_err_t error, bool snapshot_complete,
                       void *user_ctx);
    void *user_ctx; /**< Opaque context; NULL is valid. */
};

/** Core lifecycle guarded by the CHIP stack lock. */
enum fabric_core_lifecycle_t {
    FABRIC_CORE_STOPPED,
    FABRIC_CORE_RUNNING,
    FABRIC_CORE_STOPPING,
};

/**
 * @brief Core mutable state owned by the CHIP stack lock.
 *
 * The explicit installation flag is authoritative because a model listener
 * may legally use a NULL callback context.
 */
struct fabric_core {
    fabric_core_config_t config;
    nvs_handle_t nvs;

    fabric_core_lifecycle_t lifecycle;
    uint32_t active_requests;
    SemaphoreHandle_t stop_event;

    uint64_t controller_node_id;
    uint64_t fabric_id;
    uint64_t next_node_id;

    fabric_core_node *nodes;
    size_t node_count;

    core_model_listener model_listener;
    bool model_listener_installed;
};

/** Finds a commissioned node while the caller holds the CHIP stack lock. */
fabric_core_node *fabric_core_find_node(fabric_core_handle_t core, uint64_t node_id);

/** Begins one drain-counted request while the core is running. */
esp_err_t fabric_core_request_begin(fabric_core_handle_t core);
/** Ends one request and signals a waiting stop when the count reaches zero. */
void fabric_core_request_end(fabric_core_handle_t core);

/** Installs the sole model listener before the core starts. */
esp_err_t core_set_model_listener(fabric_core_handle_t core, const core_model_listener *listener);
/** Clears the matching model listener after the core stops. */
esp_err_t core_clear_model_listener(fabric_core_handle_t core, void *user_ctx);
/** Notifies the model listener that commissioned nodes changed. */
void fabric_core_private_notify_node_set_changed(fabric_core_handle_t core);

/** Initializes CHIP controller and fabric resources. */
esp_err_t fabric_core_chip_start(fabric_core_handle_t core);
/** Releases CHIP controller resources after requests drain. */
void fabric_core_chip_stop(fabric_core_handle_t core);
/** Loads and validates persisted core identity and node records. */
esp_err_t fabric_core_store_load(fabric_core_handle_t core);
/** Persists the complete commissioned node set. */
esp_err_t fabric_core_store_nodes(fabric_core_handle_t core);
/** Erases persisted Local Fabric Core records. */
esp_err_t fabric_core_store_erase(fabric_core_handle_t core);
/** Starts subscriptions for all commissioned nodes. */
esp_err_t fabric_core_subscriptions_start(fabric_core_handle_t core);
/** Stops and releases all node subscriptions. */
void fabric_core_subscriptions_stop(fabric_core_handle_t core);
/** Starts the authoritative wildcard subscription for one node. */
esp_err_t fabric_core_subscription_start_node(fabric_core_handle_t core, fabric_core_node *node);
/** Retries one node subscription that is eligible for retry. */
esp_err_t fabric_core_subscription_retry(fabric_core_handle_t core, fabric_core_node *node);

/** Dispatches commissioning through the CHIP integration. */
esp_err_t fabric_core_chip_start_commissioning(fabric_core_handle_t core, const char *setup_code,
                                               fabric_core_request_completion_cb_t completion_cb, void *user_ctx);
/** Cancels commissioning through the CHIP integration. */
esp_err_t fabric_core_chip_cancel_commissioning(fabric_core_handle_t core);
/** Dispatches a raw Matter attribute read. */
esp_err_t fabric_core_chip_read_attribute(fabric_core_handle_t core, const fabric_core_attribute_path_t *path,
                                          fabric_core_request_completion_cb_t completion_cb, void *user_ctx);
/** Dispatches a raw Matter attribute write. */
esp_err_t fabric_core_chip_write_attribute(fabric_core_handle_t core, const fabric_core_attribute_path_t *path,
                                           const fabric_core_value_t *value,
                                           fabric_core_request_completion_cb_t completion_cb, void *user_ctx);
/** Dispatches a raw Matter command invocation. */
esp_err_t fabric_core_chip_invoke_command(fabric_core_handle_t core, const fabric_core_command_path_t *path,
                                          const char *arguments_json, fabric_core_request_completion_cb_t completion_cb,
                                          void *user_ctx);
/** Dispatches commissioned-node removal through the CHIP integration. */
esp_err_t fabric_core_chip_remove_node(fabric_core_handle_t core, uint64_t node_id, bool force,
                                       fabric_core_request_completion_cb_t completion_cb, void *user_ctx);

/** Maps a CHIP error to the public ESP-IDF error domain. */
esp_err_t fabric_core_from_chip_error(CHIP_ERROR error);
