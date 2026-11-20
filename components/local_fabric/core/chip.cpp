/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_matter_controller_pairing_command.h"
#include "fabric_core_private.h"

#include "esp_check.h"
#include "esp_matter.h"
#include "esp_matter_controller_client.h"

#include <controller/CHIPDeviceController.h>
#include <controller/CommissioningDelegate.h>
#include <lib/core/DataModelTypes.h>

#include <cstring>
#include <optional>

namespace {

constexpr uint16_t kControllerListenPort = 5580;
const char *const kTag = "fabric_core_chip";

struct commissioning_context {
    fabric_core_handle_t core = nullptr;
    uint64_t node_id = 0;
    fabric_core_request_completion_cb_t completion_cb = nullptr;
    void *user_ctx = nullptr;
};

commissioning_context s_commissioning;
bool s_fabric_core_chip_initialized;

uint64_t fabric_core_next_commissioned_node_id(fabric_core_handle_t core)
{
    uint64_t candidate = core->next_node_id;
    while (candidate != UINT64_MAX) {
        core->next_node_id = candidate + 1;
        if (chip::IsOperationalNodeId(candidate) && candidate != core->controller_node_id &&
            fabric_core_find_node(core, candidate) == nullptr) {
            return candidate;
        }
        candidate = core->next_node_id;
    }
    return chip::kUndefinedNodeId;
}

void fabric_core_finish_commissioning(CHIP_ERROR chip_error)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    commissioning_context context = s_commissioning;
    fabric_core_handle_t core = context.core;
    if (core == nullptr) {
        return;
    }

    esp_err_t error = fabric_core_from_chip_error(chip_error);
    fabric_core_node *node = nullptr;
    if (error == ESP_OK) {
        if (core->node_count >= core->config.max_nodes) {
            error = ESP_ERR_NO_MEM;
        } else if (fabric_core_find_node(core, context.node_id) != nullptr) {
            error = ESP_ERR_INVALID_STATE;
        } else {
            node = &core->nodes[core->node_count++];
            node->node_id = context.node_id;
            node->subscription_state = FABRIC_CORE_SUBSCRIPTION_TERMINATED;
        }

        if (error == ESP_OK) {
            error = fabric_core_store_nodes(core);
            if (error == ESP_OK) {
                fabric_core_private_notify_node_set_changed(core);
            }
        }
        if (error != ESP_OK && node != nullptr) {
            memset(node, 0, sizeof(*node));
            --core->node_count;
            node = nullptr;
        }
    }

    bool start_subscription = error == ESP_OK && node != nullptr && core->lifecycle == FABRIC_CORE_RUNNING;
    s_commissioning = {};

    if (start_subscription) {
        esp_err_t subscription_error = fabric_core_subscription_start_node(core, node);
        if (subscription_error != ESP_OK) {
            // Commissioning succeeded and remains persisted. Subscription state
            // reports the independent connectivity failure.
        }
    }

    fabric_core_request_result_t result = {
        .operation = FABRIC_CORE_OPERATION_COMMISSION,
        .error = error,
        .node_id = context.node_id,
        .value = {},
    };
    if (context.completion_cb)
        context.completion_cb(core, &result, context.user_ctx);
    fabric_core_request_end(core);
}

void fabric_core_abandon_commissioning(fabric_core_handle_t core)
{
    s_commissioning = {};
    fabric_core_request_end(core);
}

namespace pairing = esp_matter::controller::pairing;
class commissioning_delegate final : public pairing::delegate {
    void OnDone(const pairing::completion_t &result) override { fabric_core_finish_commissioning(result.error); }
};

pairing::interaction &commissioning()
{
    static pairing::interaction interaction;
    return interaction;
}

esp_err_t reserve_commissioned_node(fabric_core_handle_t core, commissioning_context &prepared)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (core->lifecycle != FABRIC_CORE_RUNNING || s_commissioning.core != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (core->node_count >= core->config.max_nodes) {
        return ESP_ERR_NO_MEM;
    }
    prepared.node_id = fabric_core_next_commissioned_node_id(core);
    if (prepared.node_id == chip::kUndefinedNodeId) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(fabric_core_store_nodes(core), kTag, "store next commissioned node id");
    s_commissioning = prepared;
    return ESP_OK;
}

} // namespace

esp_err_t fabric_core_from_chip_error(CHIP_ERROR error)
{
    if (error == CHIP_NO_ERROR) {
        return ESP_OK;
    }
    if (error == CHIP_ERROR_NO_MEMORY) {
        return ESP_ERR_NO_MEM;
    }
    if (error == CHIP_ERROR_INVALID_ARGUMENT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (error == CHIP_ERROR_INCORRECT_STATE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (error == CHIP_ERROR_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }
    if (error == CHIP_ERROR_CANCELLED) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
}

esp_err_t fabric_core_chip_start(fabric_core_handle_t core)
{
    if (s_fabric_core_chip_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_matter::start(nullptr), kTag, "start Matter stack");

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto &client = esp_matter::controller::matter_controller_client::get_instance();
    ESP_RETURN_ON_ERROR(client.init(core->controller_node_id, core->fabric_id, kControllerListenPort), kTag,
                        "initialize controller");
    ESP_RETURN_ON_ERROR(client.setup_commissioner(), kTag, "setup commissioner");
    s_fabric_core_chip_initialized = true;
    return ESP_OK;
}

void fabric_core_chip_stop(fabric_core_handle_t core)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (s_commissioning.core == core) {
        LogErrorOnFailure(commissioning().cancel());
    }
}

esp_err_t fabric_core_chip_start_commissioning(fabric_core_handle_t core, const char *setup_code,
                                               fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    ESP_RETURN_ON_ERROR(fabric_core_request_begin(core), kTag, "admit pairing");
    commissioning_context context = {core, 0, completion_cb, user_ctx};
    esp_err_t err = reserve_commissioned_node(core, context);
    if (err != ESP_OK) {
        fabric_core_request_end(core);
        return err;
    }
    pairing::request_t request;
    request.node_id = context.node_id;
    request.params.setup_code = {setup_code, pairing::transport_t::on_network};
    static commissioning_delegate delegate;
    CHIP_ERROR result = commissioning().start(request, {}, &delegate);
    if (result != CHIP_NO_ERROR)
        fabric_core_abandon_commissioning(core);
    return fabric_core_from_chip_error(result);
}

esp_err_t fabric_core_chip_cancel_commissioning(fabric_core_handle_t core)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (s_commissioning.core != core)
        return ESP_ERR_INVALID_STATE;
    return fabric_core_from_chip_error(commissioning().cancel());
}
