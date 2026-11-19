/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_core_private.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_matter.h"

#include <platform/PlatformManager.h>

#include <cassert>
#include <cstdlib>
#include <cstring>

namespace {

const char *const kTag = "fabric_core";
portMUX_TYPE s_fabric_core_instance_lock = portMUX_INITIALIZER_UNLOCKED;
bool s_fabric_core_instance_claimed;

bool fabric_core_claim_instance()
{
    bool claimed = false;
    portENTER_CRITICAL(&s_fabric_core_instance_lock);
    if (!s_fabric_core_instance_claimed) {
        s_fabric_core_instance_claimed = true;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_fabric_core_instance_lock);
    return claimed;
}

void fabric_core_release_instance()
{
    portENTER_CRITICAL(&s_fabric_core_instance_lock);
    s_fabric_core_instance_claimed = false;
    portEXIT_CRITICAL(&s_fabric_core_instance_lock);
}

bool fabric_core_called_from_chip_context()
{
    return chip::DeviceLayer::PlatformMgr().IsChipStackLockedByCurrentThread();
}

} // namespace

esp_err_t fabric_core_create(const fabric_core_config_t *config, fabric_core_handle_t *out_core)
{
    ESP_RETURN_ON_FALSE(config && out_core, ESP_ERR_INVALID_ARG, kTag, "config and output are required");
    ESP_RETURN_ON_FALSE(config->max_nodes > 0 &&
                            config->subscription_interval_s_min <= config->subscription_interval_s_max,
                        ESP_ERR_INVALID_ARG, kTag, "invalid configuration");
    ESP_RETURN_ON_FALSE(fabric_core_claim_instance(), ESP_ERR_INVALID_STATE, kTag, "Matter controller already claimed");

    *out_core = nullptr;
    auto *core = static_cast<fabric_core_handle_t>(
        heap_caps_calloc(1, sizeof(struct fabric_core), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (core == nullptr) {
        core = static_cast<fabric_core_handle_t>(calloc(1, sizeof(*core)));
    }
    if (core == nullptr) {
        fabric_core_release_instance();
        return ESP_ERR_NO_MEM;
    }

    core->config = *config;
    core->nodes = static_cast<fabric_core_node *>(
        heap_caps_calloc(config->max_nodes, sizeof(*core->nodes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (core->nodes == nullptr) {
        core->nodes = static_cast<fabric_core_node *>(calloc(config->max_nodes, sizeof(*core->nodes)));
    }
    core->stop_event = xSemaphoreCreateBinary();
    core->next_listener_id = 1;
    if (core->nodes == nullptr || core->stop_event == nullptr) {
        if (core->stop_event != nullptr) {
            vSemaphoreDelete(core->stop_event);
        }
        free(core->nodes);
        free(core);
        fabric_core_release_instance();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = nvs_open(FABRIC_CORE_NVS_NAMESPACE, NVS_READWRITE, &core->nvs);
    if (error == ESP_OK) {
        error = fabric_core_store_load(core);
    }
    if (error == ESP_OK) {
        error = fabric_core_chip_start(core);
    }
    if (error != ESP_OK) {
        if (core->nvs != 0) {
            nvs_close(core->nvs);
        }
        vSemaphoreDelete(core->stop_event);
        free(core->nodes);
        free(core);
        fabric_core_release_instance();
        return error;
    }

    *out_core = core;
    return ESP_OK;
}

fabric_core_node *fabric_core_find_node(fabric_core_handle_t core, uint64_t node_id)
{
    for (size_t index = 0; index < core->node_count; ++index) {
        if (core->nodes[index].node_id == node_id) {
            return &core->nodes[index];
        }
    }
    return nullptr;
}

esp_err_t fabric_core_request_begin(fabric_core_handle_t core)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (core->lifecycle != FABRIC_CORE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (core->active_requests == UINT32_MAX) {
        return ESP_ERR_NO_MEM;
    }
    ++core->active_requests;
    return ESP_OK;
}

void fabric_core_request_end(fabric_core_handle_t core)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    assert(core->active_requests > 0);
    --core->active_requests;
    if (core->lifecycle == FABRIC_CORE_STOPPING && core->active_requests == 0) {
        BaseType_t given = xSemaphoreGive(core->stop_event);
        assert(given == pdTRUE);
        (void)given;
    }
}

esp_err_t fabric_core_start(fabric_core_handle_t core)
{
    ESP_RETURN_ON_FALSE(core, ESP_ERR_INVALID_ARG, kTag, "core is required");
    ESP_RETURN_ON_FALSE(!fabric_core_called_from_chip_context(), ESP_ERR_INVALID_STATE, kTag,
                        "cannot start core from a CHIP callback");

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (core->lifecycle != FABRIC_CORE_STOPPED) {
        return ESP_ERR_INVALID_STATE;
    }

    core->lifecycle = FABRIC_CORE_RUNNING;
    esp_err_t error = fabric_core_subscriptions_start(core);
    if (error != ESP_OK) {
        core->lifecycle = FABRIC_CORE_STOPPED;
    }
    return error;
}

esp_err_t fabric_core_stop(fabric_core_handle_t core)
{
    ESP_RETURN_ON_FALSE(core, ESP_ERR_INVALID_ARG, kTag, "core is required");
    ESP_RETURN_ON_FALSE(!fabric_core_called_from_chip_context(), ESP_ERR_INVALID_STATE, kTag,
                        "cannot stop core from a CHIP callback");

    bool wait_for_requests = false;
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        if (core->lifecycle != FABRIC_CORE_RUNNING) {
            return ESP_ERR_INVALID_STATE;
        }

        core->lifecycle = FABRIC_CORE_STOPPING;
        wait_for_requests = core->active_requests != 0;
        fabric_core_subscriptions_stop(core);
        fabric_core_chip_stop(core);
    }

    if (wait_for_requests) {
        BaseType_t taken = xSemaphoreTake(core->stop_event, portMAX_DELAY);
        assert(taken == pdTRUE);
        (void)taken;
    }

    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        assert(core->active_requests == 0);
        core->lifecycle = FABRIC_CORE_STOPPED;
    }
    return ESP_OK;
}

esp_err_t fabric_core_delete(fabric_core_handle_t core)
{
    ESP_RETURN_ON_FALSE(core, ESP_ERR_INVALID_ARG, kTag, "core is required");
    ESP_RETURN_ON_FALSE(!fabric_core_called_from_chip_context(), ESP_ERR_INVALID_STATE, kTag,
                        "cannot delete core from a CHIP callback");

    bool can_delete = false;
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        can_delete =
            core->lifecycle == FABRIC_CORE_STOPPED && core->active_requests == 0 && !core->private_listener_installed;
    }
    ESP_RETURN_ON_FALSE(can_delete, ESP_ERR_INVALID_STATE, kTag, "core is active");

    nvs_close(core->nvs);
    vSemaphoreDelete(core->stop_event);
    free(core->nodes);
    free(core);
    fabric_core_release_instance();
    return ESP_OK;
}

esp_err_t fabric_core_private_set_listener(fabric_core_handle_t core, const fabric_core_private_listener *listener)
{
    ESP_RETURN_ON_FALSE(core && listener, ESP_ERR_INVALID_ARG, kTag, "private listener is required");
    ESP_RETURN_ON_FALSE(!fabric_core_called_from_chip_context(), ESP_ERR_INVALID_STATE, kTag,
                        "cannot set listener from a CHIP callback");

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (core->private_listener_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    core->private_listener = *listener;
    core->private_listener_installed = true;
    return ESP_OK;
}

esp_err_t fabric_core_private_clear_listener(fabric_core_handle_t core, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(core, ESP_ERR_INVALID_ARG, kTag, "core is required");
    ESP_RETURN_ON_FALSE(!fabric_core_called_from_chip_context(), ESP_ERR_INVALID_STATE, kTag,
                        "cannot clear listener from a CHIP callback");

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (!core->private_listener_installed || core->private_listener.user_ctx != user_ctx) {
        return ESP_ERR_NOT_FOUND;
    }
    memset(&core->private_listener, 0, sizeof(core->private_listener));
    core->private_listener_installed = false;
    return ESP_OK;
}

esp_err_t fabric_core_add_listener(fabric_core_handle_t core, const fabric_core_listener_t *listener,
                                   fabric_core_listener_id_t *out_listener_id)
{
    ESP_RETURN_ON_FALSE(core && listener && out_listener_id, ESP_ERR_INVALID_ARG, kTag,
                        "listener and output are required");
    ESP_RETURN_ON_FALSE(!fabric_core_called_from_chip_context(), ESP_ERR_INVALID_STATE, kTag,
                        "cannot add listener from a CHIP callback");

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    for (auto &entry : core->listeners) {
        if (entry.id == 0) {
            if (core->next_listener_id == 0) {
                return ESP_ERR_NO_MEM;
            }
            entry.id = core->next_listener_id++;
            entry.listener = *listener;
            *out_listener_id = entry.id;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t fabric_core_remove_listener(fabric_core_handle_t core, fabric_core_listener_id_t listener_id)
{
    ESP_RETURN_ON_FALSE(core && listener_id, ESP_ERR_INVALID_ARG, kTag, "core and listener id are required");
    ESP_RETURN_ON_FALSE(!fabric_core_called_from_chip_context(), ESP_ERR_INVALID_STATE, kTag,
                        "cannot remove listener from a CHIP callback");

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    for (auto &entry : core->listeners) {
        if (entry.id == listener_id) {
            memset(&entry, 0, sizeof(entry));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t fabric_core_get_node_ids(fabric_core_handle_t core, uint64_t *node_ids, size_t capacity, size_t *out_count)
{
    ESP_RETURN_ON_FALSE(core && out_count && (capacity == 0 || node_ids), ESP_ERR_INVALID_ARG, kTag, "invalid output");

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    *out_count = core->node_count;
    if (capacity < core->node_count) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t index = 0; index < core->node_count; ++index) {
        node_ids[index] = core->nodes[index].node_id;
    }
    return ESP_OK;
}

esp_err_t fabric_core_start_commissioning(fabric_core_handle_t core, const char *setup_code,
                                          fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(core && setup_code, ESP_ERR_INVALID_ARG, kTag, "invalid commissioning request");
    ESP_RETURN_ON_FALSE(strlen(setup_code) <= FABRIC_CORE_MAX_SETUP_CODE_LENGTH, ESP_ERR_INVALID_SIZE, kTag,
                        "setup code too long");

    return fabric_core_chip_start_commissioning(core, setup_code, completion_cb, user_ctx);
}

esp_err_t fabric_core_cancel_commissioning(fabric_core_handle_t core)
{
    ESP_RETURN_ON_FALSE(core, ESP_ERR_INVALID_ARG, kTag, "core is required");
    return fabric_core_chip_cancel_commissioning(core);
}

esp_err_t fabric_core_read_attribute(fabric_core_handle_t core, const fabric_core_attribute_path_t *path,
                                     fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(core && path, ESP_ERR_INVALID_ARG, kTag, "path is required");
    return fabric_core_chip_read_attribute(core, path, completion_cb, user_ctx);
}

esp_err_t fabric_core_write_attribute(fabric_core_handle_t core, const fabric_core_attribute_path_t *path,
                                      const fabric_core_value_t *value,
                                      fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(core && path && value, ESP_ERR_INVALID_ARG, kTag, "path and value are required");
    return fabric_core_chip_write_attribute(core, path, value, completion_cb, user_ctx);
}

esp_err_t fabric_core_invoke_command(fabric_core_handle_t core, const fabric_core_command_path_t *path,
                                     const char *arguments_json, fabric_core_request_completion_cb_t completion_cb,
                                     void *user_ctx)
{
    ESP_RETURN_ON_FALSE(core && path, ESP_ERR_INVALID_ARG, kTag, "path is required");
    return fabric_core_chip_invoke_command(core, path, arguments_json ? arguments_json : "{}", completion_cb, user_ctx);
}

esp_err_t fabric_core_remove_node(fabric_core_handle_t core, uint64_t node_id, bool force,
                                  fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(core && node_id, ESP_ERR_INVALID_ARG, kTag, "node id is required");
    return fabric_core_chip_remove_node(core, node_id, force, completion_cb, user_ctx);
}

esp_err_t fabric_core_factory_reset(fabric_core_handle_t core)
{
    ESP_RETURN_ON_FALSE(core, ESP_ERR_INVALID_ARG, kTag, "core is required");

    bool stopped = false;
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        stopped = core->lifecycle == FABRIC_CORE_STOPPED;
    }
    ESP_RETURN_ON_FALSE(stopped, ESP_ERR_INVALID_STATE, kTag, "stop core first");
    return fabric_core_store_erase(core);
}

esp_err_t fabric_core_retry_node_subscription(fabric_core_handle_t core, uint64_t node_id)
{
    ESP_RETURN_ON_FALSE(core && node_id, ESP_ERR_INVALID_ARG, kTag, "core and node id are required");

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (core->lifecycle != FABRIC_CORE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    fabric_core_node *node = fabric_core_find_node(core, node_id);
    return node != nullptr ? fabric_core_subscription_retry(core, node) : ESP_ERR_NOT_FOUND;
}
