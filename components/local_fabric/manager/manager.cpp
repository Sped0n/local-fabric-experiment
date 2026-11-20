/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_manager_private.h"

#include <cassert>
#include <cstdio>

#include "esp_check.h"
#include "esp_log.h"

#include <cstdlib>
#include <cstring>

namespace {
const char *const kTag = "fabric_manager";

data_model::device *find_device(data_model::device_set &set, fabric_manager_device_id_t device_id)
{
    for (size_t index = 0; index < set.device_count; ++index) {
        if (set.devices[index].id == device_id)
            return &set.devices[index];
    }
    return nullptr;
}

const data_model::cluster_adapter *find_cluster_state(data_model::device_set &set, fabric_manager_device_id_t device_id,
                                                      uint16_t endpoint_id, const char *cluster_key,
                                                      uint64_t *out_node_id, data_model::cluster_state *out_state)
{
    const auto *device = find_device(set, device_id);
    if (!device)
        return nullptr;
    *out_node_id = device->node_id;
    for (size_t endpoint_offset = 0; endpoint_offset < device->endpoint_count; ++endpoint_offset) {
        const auto &endpoint = set.endpoints[device->first_endpoint + endpoint_offset];
        if (endpoint.endpoint_id != endpoint_id)
            continue;
        for (size_t cluster_offset = 0; cluster_offset < endpoint.cluster_count; ++cluster_offset) {
            const auto &cluster = set.clusters[endpoint.first_cluster + cluster_offset];
            if (!strcmp(cluster.adapter->key, cluster_key)) {
                *out_state = cluster.state;
                return cluster.adapter;
            }
        }
        return nullptr;
    }
    return nullptr;
}

void on_node_set_changed(fabric_core_handle_t, void *user_ctx)
{
    auto manager = static_cast<fabric_manager_handle_t>(user_ctx);
    uint64_t *node_ids = nullptr;
    size_t node_count = 0;
    esp_err_t error = fabric_core_get_node_ids(manager->core, nullptr, 0, &node_count);
    if (error != ESP_OK && error != ESP_ERR_INVALID_SIZE)
        return;
    if (node_count != 0) {
        node_ids = static_cast<uint64_t *>(calloc(node_count, sizeof(uint64_t)));
        if (!node_ids)
            return;
        error = fabric_core_get_node_ids(manager->core, node_ids, node_count, &node_count);
        if (error != ESP_OK) {
            free(node_ids);
            return;
        }
    }

    data_model::device_set filtered = {};
    bool changed = false;
    {
        data_model::lock_guard lock(manager->state_mutex);
        error = data_model::model::retain_nodes(manager, node_ids, node_count, &filtered);
        changed = error == ESP_OK && filtered.device_count != manager->set.device_count;
        if (changed) {
            error = data_model::store::save(manager, filtered.devices, filtered.device_count, manager->next_device_id);
        }
        if (error == ESP_OK && changed) {
            data_model::device_set previous = manager->set;
            manager->set = filtered;
            manager_signal_changes(manager, FABRIC_DEVICE_METADATA | FABRIC_DEVICE_STATE);
            filtered = {};
            data_model::model::free(&previous);
        }
    }
    data_model::model::free(&filtered);
    free(node_ids);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "failed to reconcile nodes: %s", esp_err_to_name(error));
    }
}

void on_subscription_changed(fabric_core_handle_t, const fabric_core_subscription_update_t *update, void *user_ctx)
{
    auto manager = static_cast<fabric_manager_handle_t>(user_ctx);
    if (!manager || !update)
        return;
    const bool online = update->state == FABRIC_CORE_SUBSCRIPTION_ACTIVE;
    bool changed = false;
    {
        data_model::lock_guard lock(manager->state_mutex);
        for (size_t index = 0; index < manager->set.device_count; ++index) {
            auto &device = manager->set.devices[index];
            if (device.node_id == update->node_id && device.online != online) {
                device.online = online;
                device.baseline_epoch = ++manager->next_epoch;
                changed = true;
            }
        }
        if (changed)
            manager_signal_changes(manager, FABRIC_DEVICE_STATE, update->node_id);
    }
}

} // namespace

void manager_signal_changes(fabric_manager_handle_t manager, unsigned views, uint64_t node_id)
{
    uint32_t mask = node_id ? 0 : UINT32_MAX;
    for (size_t i = 0; node_id && i < manager->set.device_count; i++)
        if (manager->set.devices[i].node_id == node_id)
            mask |= 1u << (manager->set.devices[i].id % 32);
    for (auto &s : manager->signals)
        if (s.notify)
            s.notify(s.ctx, views, mask ? mask : UINT32_MAX);
}

esp_err_t fabric_manager_create(fabric_core_handle_t core, fabric_manager_handle_t *out_manager)
{
    ESP_RETURN_ON_FALSE(core && out_manager, ESP_ERR_INVALID_ARG, kTag, "invalid create arguments");
    *out_manager = nullptr;
    auto manager = static_cast<fabric_manager_handle_t>(calloc(1, sizeof(struct fabric_manager)));
    ESP_RETURN_ON_FALSE(manager, ESP_ERR_NO_MEM, kTag, "allocate device manager");
    manager->core = core;
    manager->state_mutex = xSemaphoreCreateMutex();
    if (!manager->state_mutex) {
        fabric_manager_delete(manager);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = nvs_open(FABRIC_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &manager->nvs);
    if (error != ESP_OK) {
        fabric_manager_delete(manager);
        return error;
    }
    error = data_model::store::load(manager, &manager->set.devices, &manager->set.device_count);
    if (error != ESP_OK) {
        fabric_manager_delete(manager);
        return error;
    }
    const core_model_listener listener = {
        .node_set_changed = on_node_set_changed,
        .subscription_changed = on_subscription_changed,
        .report_begin = data_model::report_begin,
        .attribute_reported = data_model::attribute_reported,
        .report_end = data_model::report_end,
        .user_ctx = manager,
    };
    error = core_set_model_listener(core, &listener);
    if (error != ESP_OK) {
        fabric_manager_delete(manager);
        return error;
    }
    *out_manager = manager;
    return ESP_OK;
}

esp_err_t fabric_manager_delete(fabric_manager_handle_t manager)
{
    if (!manager)
        return ESP_ERR_INVALID_ARG;
    for (const auto &s : manager->signals)
        if (s.notify)
            return ESP_ERR_INVALID_STATE;
    if (manager->core) {
        const esp_err_t error = core_clear_model_listener(manager->core, manager);
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND)
            return error;
    }
    while (manager->reports) {
        auto *transaction = manager->reports;
        manager->reports = transaction->next;
        data_model::free_report(transaction);
    }
    data_model::model::free(&manager->set);
    if (manager->nvs)
        nvs_close(manager->nvs);
    if (manager->state_mutex)
        vSemaphoreDelete(manager->state_mutex);
    free(manager);
    return ESP_OK;
}

esp_err_t fabric_manager_add_signal(fabric_manager_handle_t m, fabric_change_signal_t signal, void *ctx)
{
    if (!m || !signal)
        return ESP_ERR_INVALID_ARG;
    data_model::lock_guard lock(m->state_mutex);
    for (auto &s : m->signals)
        if (!s.notify) {
            s = {signal, ctx};
            return ESP_OK;
        }
    return ESP_ERR_NO_MEM;
}
void fabric_manager_remove_signal(fabric_manager_handle_t m, fabric_change_signal_t signal, void *ctx)
{
    data_model::lock_guard lock(m->state_mutex);
    for (auto &s : m->signals)
        if (s.notify == signal && s.ctx == ctx)
            s = {};
}
namespace {
struct property_copy {
    data_model::cluster_state state;
    const data_model::cluster_adapter *adapter;
    bool online;
};
esp_err_t encode_property(const property_copy &copy, const char *key, cJSON **out)
{
    cJSON *props = cJSON_CreateObject(), *actions = cJSON_CreateObject();
    esp_err_t err = props && actions ? copy.adapter->encode_json(&copy.state, props, actions) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        *out = cJSON_DetachItemFromObjectCaseSensitive(props, key);
        if (!*out)
            err = ESP_ERR_NOT_FOUND;
        else if (!copy.online) {
            cJSON_DeleteItemFromObjectCaseSensitive(*out, "value");
            if (!cJSON_AddNullToObject(*out, "value")) {
                cJSON_Delete(*out);
                *out = nullptr;
                err = ESP_ERR_NO_MEM;
            }
        }
    }
    cJSON_Delete(props);
    cJSON_Delete(actions);
    return err;
}
} // namespace
static esp_err_t read_properties(fabric_manager_handle_t m, const fabric_property_path_t *const *paths, size_t count,
                                 cJSON **out, uint32_t *epochs, property_copy *copies)
{
    if (!m || !paths || !out || !count || count > FABRIC_MANAGER_PROPERTY_BATCH_MAX)
        return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; i++)
        if (!paths[i])
            return ESP_ERR_INVALID_ARG;
    {
        data_model::lock_guard lock(m->state_mutex);
        for (size_t i = 0; i < count; i++) {
            out[i] = nullptr;
            auto *p = paths[i];
            auto *d = find_device(m->set, p->device);
            if (epochs)
                epochs[i] = d ? d->baseline_epoch : 0;
            if (!d)
                continue;
            uint64_t node;
            copies[i].online = d->online;
            copies[i].adapter = find_cluster_state(m->set, p->device, p->endpoint, p->cluster, &node, &copies[i].state);
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (!copies[i].adapter)
            continue;
        esp_err_t err = encode_property(copies[i], paths[i]->property, &out[i]);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            for (size_t j = 0; j < count; j++) {
                cJSON_Delete(out[j]);
                out[j] = nullptr;
            }
            return err;
        }
    }
    return ESP_OK;
}
esp_err_t fabric_manager_properties(fabric_manager_handle_t m, const fabric_property_path_t *const *paths, size_t count,
                                    cJSON **out, uint32_t *epochs)
{
    property_copy copies[FABRIC_MANAGER_PROPERTY_BATCH_MAX]{};
    return read_properties(m, paths, count, out, epochs, copies);
}
esp_err_t fabric_manager_property(fabric_manager_handle_t m, const fabric_property_path_t *p, cJSON **out,
                                  uint32_t *epoch)
{
    if (!p)
        return ESP_ERR_INVALID_ARG;
    property_copy copy{};
    esp_err_t err = read_properties(m, &p, 1, out, epoch, &copy);
    return err == ESP_OK && !*out ? ESP_ERR_NOT_FOUND : err;
}
esp_err_t fabric_manager_devices_json(fabric_manager_handle_t manager, fabric_device_view_t view, char **out)
{
    if (!manager || !out || (view != FABRIC_DEVICE_METADATA && view != FABRIC_DEVICE_STATE))
        return ESP_ERR_INVALID_ARG;
    data_model::lock_guard lock(manager->state_mutex);
    *out = data_model::json::devices(manager->set, view);
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t fabric_manager_snapshot_json(fabric_manager_handle_t manager, char **out)
{
    if (!manager || !out)
        return ESP_ERR_INVALID_ARG;
    data_model::lock_guard lock(manager->state_mutex);
    *out = data_model::json::list(manager->set, true);
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t fabric_manager_list_devices_json(fabric_manager_handle_t manager, char **out_json)
{
    ESP_RETURN_ON_FALSE(manager && out_json, ESP_ERR_INVALID_ARG, kTag, "invalid list output");
    *out_json = nullptr;
    {
        data_model::lock_guard lock(manager->state_mutex);
        *out_json = data_model::json::list(manager->set);
    }
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t fabric_manager_get_device_json(fabric_manager_handle_t manager, fabric_manager_device_id_t device_id,
                                         char **out_json)
{
    ESP_RETURN_ON_FALSE(manager && device_id != 0 && out_json, ESP_ERR_INVALID_ARG, kTag, "invalid detail output");
    *out_json = nullptr;
    bool found = false;
    {
        data_model::lock_guard lock(manager->state_mutex);
        const auto *device = find_device(manager->set, device_id);
        found = device != nullptr;
        if (device)
            *out_json = data_model::json::detail(manager->set, *device);
    }
    if (!found)
        return ESP_ERR_NOT_FOUND;
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

void fabric_manager_free_json(char *json)
{
    cJSON_free(json);
}

esp_err_t fabric_manager_set_device_identity(fabric_manager_handle_t manager, fabric_manager_device_id_t device_id,
                                             const char *name, const char *room)
{
    ESP_RETURN_ON_FALSE(manager && device_id != 0 && name && *name && room, ESP_ERR_INVALID_ARG, kTag,
                        "invalid identity");
    ESP_RETURN_ON_FALSE(strnlen(name, FABRIC_MANAGER_NAME_MAX) < FABRIC_MANAGER_NAME_MAX &&
                            strnlen(room, FABRIC_MANAGER_ROOM_MAX) < FABRIC_MANAGER_ROOM_MAX,
                        ESP_ERR_INVALID_SIZE, kTag, "identity too long");
    esp_err_t error = ESP_OK;
    {
        data_model::lock_guard lock(manager->state_mutex);
        auto *device = find_device(manager->set, device_id);
        if (!device)
            return ESP_ERR_NOT_FOUND;
        if (!strcmp(device->name, name) && !strcmp(device->room, room))
            return ESP_OK;
        const auto previous = *device;
        snprintf(device->name, sizeof(device->name), "%s", name);
        snprintf(device->room, sizeof(device->room), "%s", room);
        error =
            data_model::store::save(manager, manager->set.devices, manager->set.device_count, manager->next_device_id);
        if (error != ESP_OK)
            *device = previous;
        else
            manager_signal_changes(manager, FABRIC_DEVICE_METADATA, device->node_id);
    }
    return error;
}

esp_err_t fabric_manager_invoke_action(fabric_manager_handle_t manager, fabric_manager_device_id_t device_id,
                                       uint16_t endpoint_id, const char *cluster_key, const char *action_key,
                                       const char *arguments_json, fabric_core_request_completion_cb_t completion_cb,
                                       void *user_ctx)
{
    ESP_RETURN_ON_FALSE(manager && device_id != 0 && endpoint_id != 0 && cluster_key && action_key && arguments_json,
                        ESP_ERR_INVALID_ARG, kTag, "invalid action");
    cJSON *arguments = cJSON_Parse(arguments_json);
    if (!cJSON_IsObject(arguments)) {
        cJSON_Delete(arguments);
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t node_id = 0;
    const data_model::cluster_adapter *adapter = nullptr;
    data_model::cluster_state cluster_state = {};
    {
        data_model::lock_guard lock(manager->state_mutex);
        adapter = find_cluster_state(manager->set, device_id, endpoint_id, cluster_key, &node_id, &cluster_state);
    }
    if (!adapter || !adapter->invoke_action) {
        cJSON_Delete(arguments);
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t error = adapter->invoke_action(manager->core, node_id, endpoint_id, &cluster_state, action_key,
                                                   arguments, completion_cb, user_ctx);
    cJSON_Delete(arguments);
    return error;
}

esp_err_t fabric_manager_remove_device(fabric_manager_handle_t manager, fabric_manager_device_id_t device_id,
                                       bool force, fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(manager && device_id != 0, ESP_ERR_INVALID_ARG, kTag, "invalid removal");
    uint64_t node_id = 0;
    {
        data_model::lock_guard lock(manager->state_mutex);
        const auto *device = find_device(manager->set, device_id);
        if (device)
            node_id = device->node_id;
    }
    if (!node_id)
        return ESP_ERR_NOT_FOUND;
    return fabric_core_remove_node(manager->core, node_id, force, completion_cb, user_ctx);
}
