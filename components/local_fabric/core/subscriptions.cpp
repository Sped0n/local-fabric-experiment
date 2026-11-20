/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_core_private.h"

#include "esp_matter.h"
#include "esp_matter_controller_client.h"

#include <app/BufferedReadCallback.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <app/ReadPrepareParams.h>
#include <lib/core/TLV.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/PlatformManager.h>

#include <utility>

namespace {

using chip::app::AttributePathParams;
using chip::app::ConcreteDataAttributePath;
using chip::app::InteractionModelEngine;
using chip::app::ReadClient;
using chip::app::ReadPrepareParams;
using chip::app::StatusIB;

struct fabric_core_subscription final : ReadClient::Callback {
    fabric_core_handle_t core;
    fabric_core_node *node;
    AttributePathParams wildcard_path;
    chip::app::BufferedReadCallback buffered{*this};
    ReadClient *client = nullptr;
    bool established = false;
    bool priming_report_complete = false;
    bool report_open = false;
    CHIP_ERROR report_error = CHIP_NO_ERROR;

    fabric_core_subscription(fabric_core_handle_t core_handle, fabric_core_node *node_entry)
        : core(core_handle)
        , node(node_entry)
        , wildcard_path()
    {
    }

    CHIP_ERROR start()
    {
        if (client != nullptr) {
            return CHIP_ERROR_INCORRECT_STATE;
        }
        established = false;
        priming_report_complete = false;
        report_open = false;
        report_error = CHIP_NO_ERROR;
        wildcard_path = AttributePathParams();
        client = chip::Platform::New<ReadClient>(InteractionModelEngine::GetInstance(), nullptr, buffered,
                                                 ReadClient::InteractionType::Subscribe);
        VerifyOrReturnError(client != nullptr, CHIP_ERROR_NO_MEMORY);

        ReadPrepareParams params;
        params.mpAttributePathParamsList = &wildcard_path;
        params.mAttributePathParamsListSize = 1;
        params.mMinIntervalFloorSeconds = core->config.subscription_interval_s_min;
        params.mMaxIntervalCeilingSeconds = core->config.subscription_interval_s_max;
        params.mKeepSubscriptions = true;
        const chip::ScopedNodeId peer(
            node->node_id, esp_matter::controller::matter_controller_client::get_instance().get_fabric_index());
        CHIP_ERROR error = client->SendAutoResubscribeRequest(peer, std::move(params));
        if (error != CHIP_NO_ERROR) {
            // SendAutoResubscribeRequest has already returned retained paths.
            chip::Platform::Delete(client);
            client = nullptr;
        }
        return error;
    }

    void stop()
    {
        if (client != nullptr) {
            chip::Platform::Delete(client);
            client = nullptr;
        }
        established = false;
        priming_report_complete = false;
        report_open = false;
    }

    void OnReportBegin() override;
    void OnReportEnd() override;
    void OnAttributeData(const ConcreteDataAttributePath &path, chip::TLV::TLVReader *data,
                         const StatusIB &status) override;
    void OnSubscriptionEstablished(chip::SubscriptionId subscription_id) override;
    CHIP_ERROR OnResubscriptionNeeded(ReadClient *read_client, CHIP_ERROR cause) override;
    void OnError(CHIP_ERROR error) override;
    void OnDone(ReadClient *read_client) override;
    void OnDeallocatePaths(ReadPrepareParams &&params) override { (void)params; }
};

template <typename Callback>
void core_dispatch_model_listener(fabric_core_handle_t core, Callback callback)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (core->lifecycle == FABRIC_CORE_RUNNING && core->model_listener_installed) {
        callback(core->model_listener);
    }
}

void fabric_core_set_subscription_state(fabric_core_subscription &subscription, fabric_core_subscription_state_t state,
                                        CHIP_ERROR cause = CHIP_NO_ERROR)
{
    fabric_core_handle_t core = subscription.core;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (subscription.node->subscription_state == state) {
        return;
    }
    subscription.node->subscription_state = state;
    const fabric_core_subscription_update_t update = {
        .node_id = subscription.node->node_id,
        .state = state,
        .cause = fabric_core_from_chip_error(cause),
    };
    core_dispatch_model_listener(core, [&](const core_model_listener &listener) {
        if (listener.subscription_changed != nullptr) {
            listener.subscription_changed(core, &update, listener.user_ctx);
        }
    });
}

void fabric_core_subscription::OnReportBegin()
{
    // A priming subscription report can span several ReportData messages.
    // Keep one transaction open until the SubscribeResponse confirms that the
    // complete snapshot has arrived.
    if (report_open) {
        return;
    }
    report_open = true;
    report_error = CHIP_NO_ERROR;
    core_dispatch_model_listener(core, [&](const core_model_listener &listener) {
        if (listener.report_begin != nullptr) {
            listener.report_begin(core, node->node_id, !priming_report_complete, listener.user_ctx);
        }
    });
}

void fabric_core_subscription::OnReportEnd()
{
    if (!report_open) {
        return;
    }
    if (!established && !priming_report_complete) {
        return;
    }
    report_open = false;
    core_dispatch_model_listener(core, [&](const core_model_listener &listener) {
        if (listener.report_end != nullptr) {
            listener.report_end(core, node->node_id, fabric_core_from_chip_error(report_error), !priming_report_complete,
                                listener.user_ctx);
        }
    });
    if (report_error == CHIP_NO_ERROR) {
        priming_report_complete = true;
        if (established) {
            fabric_core_set_subscription_state(*this, FABRIC_CORE_SUBSCRIPTION_ACTIVE);
        }
    }
}

void fabric_core_subscription::OnAttributeData(const ConcreteDataAttributePath &path, chip::TLV::TLVReader *data,
                                               const StatusIB &status)
{
    // Per-path failures do not invalidate the wildcard report transaction.
    const fabric_core_private_attribute_report report = {
        .path =
            {
                .endpoint = {.node_id = node->node_id, .endpoint_id = path.mEndpointId},
                .cluster_id = path.mClusterId,
                .attribute_id = path.mAttributeId,
            },
        .data = data,
        .error = status.ToChipError(),
    };
    core_dispatch_model_listener(core, [&](const core_model_listener &listener) {
        if (listener.attribute_reported != nullptr) {
            listener.attribute_reported(core, &report, listener.user_ctx);
        }
    });
}

void fabric_core_subscription::OnSubscriptionEstablished(chip::SubscriptionId subscription_id)
{
    (void)subscription_id;
    established = true;
    if (report_open && !priming_report_complete) {
        OnReportEnd();
    } else if (priming_report_complete) {
        fabric_core_set_subscription_state(*this, FABRIC_CORE_SUBSCRIPTION_ACTIVE);
    }
}

CHIP_ERROR fabric_core_subscription::OnResubscriptionNeeded(ReadClient *read_client, CHIP_ERROR cause)
{
    established = false;
    priming_report_complete = false;
    fabric_core_set_subscription_state(*this, FABRIC_CORE_SUBSCRIPTION_BACKOFF, cause);
    return read_client->DefaultResubscribePolicy(cause);
}

void fabric_core_subscription::OnError(CHIP_ERROR error)
{
    if (report_open) {
        report_open = false;
        core_dispatch_model_listener(core, [&](const core_model_listener &listener) {
            if (listener.report_end != nullptr) {
                listener.report_end(core, node->node_id, fabric_core_from_chip_error(error), false, listener.user_ctx);
            }
        });
    }
    report_error = error;
}

void fabric_core_subscription::OnDone(ReadClient *read_client)
{
    if (client == read_client) {
        chip::Platform::Delete(client);
        client = nullptr;
    }
    established = false;
    priming_report_complete = false;
    fabric_core_set_subscription_state(*this, FABRIC_CORE_SUBSCRIPTION_TERMINATED, report_error);
}

fabric_core_subscription *fabric_core_get_subscription(fabric_core_node *node)
{
    return static_cast<fabric_core_subscription *>(node->subscription);
}

} // namespace

void fabric_core_private_notify_node_set_changed(fabric_core_handle_t core)
{
    core_dispatch_model_listener(core, [&](const core_model_listener &listener) {
        if (listener.node_set_changed != nullptr) {
            listener.node_set_changed(core, listener.user_ctx);
        }
    });
}

esp_err_t fabric_core_subscription_start_node(fabric_core_handle_t core, fabric_core_node *node)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto *subscription = fabric_core_get_subscription(node);
    if (subscription == nullptr) {
        subscription = chip::Platform::New<fabric_core_subscription>(core, node);
        if (subscription == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        node->subscription = subscription;
    }
    fabric_core_set_subscription_state(*subscription, FABRIC_CORE_SUBSCRIPTION_CONNECTING);
    CHIP_ERROR error = subscription->start();
    if (error != CHIP_NO_ERROR) {
        fabric_core_set_subscription_state(*subscription, FABRIC_CORE_SUBSCRIPTION_TERMINATED, error);
    }
    return fabric_core_from_chip_error(error);
}

esp_err_t fabric_core_subscriptions_start(fabric_core_handle_t core)
{
    for (size_t index = 0; index < core->node_count; ++index) {
        esp_err_t error = fabric_core_subscription_start_node(core, &core->nodes[index]);
        if (error != ESP_OK) {
            fabric_core_subscriptions_stop(core);
            return error;
        }
    }
    return ESP_OK;
}

void fabric_core_subscriptions_stop(fabric_core_handle_t core)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    for (size_t index = 0; index < core->node_count; ++index) {
        auto *subscription = fabric_core_get_subscription(&core->nodes[index]);
        if (subscription != nullptr) {
            subscription->stop();
            chip::Platform::Delete(subscription);
            core->nodes[index].subscription = nullptr;
        }
        core->nodes[index].subscription_state = FABRIC_CORE_SUBSCRIPTION_TERMINATED;
    }
}

esp_err_t fabric_core_subscription_retry(fabric_core_handle_t core, fabric_core_node *node)
{
    (void)core;
    if (node->subscription_state == FABRIC_CORE_SUBSCRIPTION_CONNECTING ||
        node->subscription_state == FABRIC_CORE_SUBSCRIPTION_ACTIVE) {
        return ESP_OK;
    }
    if (node->subscription_state == FABRIC_CORE_SUBSCRIPTION_TERMINATED) {
        return fabric_core_subscription_start_node(core, node);
    }
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto *subscription = fabric_core_get_subscription(node);
    if (subscription == nullptr || subscription->client == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return subscription->client->TriggerResubscribeIfScheduled("Local Fabric manual retry") ? ESP_OK
                                                                                            : ESP_ERR_INVALID_STATE;
}
