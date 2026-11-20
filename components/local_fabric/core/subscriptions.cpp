/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_core_private.h"

#include "esp_matter_client_subscribe_command.h"
#include "esp_matter_controller_client.h"

#include <lib/support/CHIPMem.h>

namespace subscribe = esp_matter::client::subscribe_command;
using esp_matter::client::target_t;

// The SDK owns connectivity and path storage. Only the model's report transaction spans callbacks.
struct fabric_core_subscription final : subscribe::delegate {
    fabric_core_subscription(fabric_core_handle_t core, uint64_t node_id)
        : core(core)
        , node_id(node_id)
    {
    }

    CHIP_ERROR start()
    {
        chip::app::AttributePathParams wildcard;
        subscribe::options_t options;
        options.min_interval = core->config.subscription_interval_s_min;
        options.max_interval = core->config.subscription_interval_s_max;
        const chip::ScopedNodeId peer(
            node_id, esp_matter::controller::matter_controller_client::get_instance().get_fabric_index());
        return handle.start(peer, subscribe::request_t({&wildcard, 1}, options), *this);
    }

    void stop()
    {
        handle.stop(); // Detaches the delegate; no terminal callback follows.
        offline(CHIP_ERROR_CANCELLED);
    }

    subscribe::subscription handle;

private:
    void OnReportBegin(const target_t &) override
    {
        const auto &listener = core->model_listener;
        if (!report_open && core->model_listener_installed && listener.report_begin) {
            report_open = true;
            listener.report_begin(core, node_id, handle.state() != subscribe::state_t::active, listener.user_ctx);
        }
    }

    void OnAttribute(const target_t &, const chip::app::ConcreteDataAttributePath &path, chip::TLV::TLVReader *value,
                     const chip::app::StatusIB &status) override
    {
        const auto &listener = core->model_listener;
        if (report_open && listener.attribute_reported) {
            const fabric_core_private_attribute_report report = {
                .path = {.endpoint = {.node_id = node_id, .endpoint_id = path.mEndpointId},
                         .cluster_id = path.mClusterId,
                         .attribute_id = path.mAttributeId},
                .data = value,
                .error = status.ToChipError(),
            };
            listener.attribute_reported(core, &report, listener.user_ctx);
        }
    }

    void OnReportEnd(const target_t &, CHIP_ERROR error) override
    {
        // Priming can span multiple report boundaries before the SDK confirms establishment.
        if (error != CHIP_NO_ERROR || handle.state() == subscribe::state_t::active)
            finish_report(error, false);
    }

    void OnStateChanged(const target_t &, subscribe::state_t state, CHIP_ERROR error) override
    {
        if (state == subscribe::state_t::active)
            finish_report(CHIP_NO_ERROR, true);
        else if (state == subscribe::state_t::backoff || state == subscribe::state_t::terminated)
            offline(error);
    }

    void finish_report(CHIP_ERROR error, bool snapshot_complete)
    {
        if (!report_open)
            return;
        report_open = false;
        const auto &listener = core->model_listener;
        if (listener.report_end)
            listener.report_end(core, node_id, fabric_core_from_chip_error(error), snapshot_complete,
                                listener.user_ctx);
    }

    void offline(CHIP_ERROR error)
    {
        finish_report(error == CHIP_NO_ERROR ? CHIP_ERROR_CONNECTION_ABORTED : error, false);
        const auto &listener = core->model_listener;
        if (core->model_listener_installed && listener.node_offline)
            listener.node_offline(core, node_id, listener.user_ctx);
    }

    fabric_core_handle_t core;
    uint64_t node_id;
    bool report_open = false;
};

esp_err_t fabric_core_subscription_start_node(fabric_core_handle_t core, fabric_core_node *node)
{
    if (!node->subscription) {
        node->subscription = chip::Platform::New<fabric_core_subscription>(core, node->node_id);
        if (!node->subscription)
            return ESP_ERR_NO_MEM;
    }
    return fabric_core_from_chip_error(node->subscription->start());
}

esp_err_t fabric_core_subscriptions_start(fabric_core_handle_t core)
{
    for (size_t i = 0; i < core->node_count; ++i) {
        const esp_err_t error = fabric_core_subscription_start_node(core, &core->nodes[i]);
        if (error != ESP_OK) {
            fabric_core_subscriptions_stop(core);
            return error;
        }
    }
    return ESP_OK;
}

void fabric_core_subscriptions_stop(fabric_core_handle_t core)
{
    for (size_t i = 0; i < core->node_count; ++i) {
        auto &subscription = core->nodes[i].subscription;
        if (subscription) {
            subscription->stop();
            chip::Platform::Delete(subscription);
            subscription = nullptr;
        }
    }
}

esp_err_t fabric_core_subscription_retry(fabric_core_handle_t core, fabric_core_node *node)
{
    auto *subscription = node->subscription;
    if (!subscription || subscription->handle.state() == subscribe::state_t::terminated)
        return fabric_core_subscription_start_node(core, node);
    if (subscription->handle.state() == subscribe::state_t::backoff)
        return fabric_core_from_chip_error(subscription->handle.retry_now());
    return ESP_OK; // Already connecting or active.
}
