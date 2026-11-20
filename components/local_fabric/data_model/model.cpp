/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_manager_private.h"

#include "clusters/boolean_state/boolean_state.h"
#include "clusters/color_control/color_control.h"
#include "clusters/fan_control/fan_control.h"
#include "clusters/illuminance_measurement/illuminance_measurement.h"
#include "clusters/level_control/level_control.h"
#include "clusters/occupancy_sensing/occupancy_sensing.h"
#include "clusters/on_off/on_off.h"
#include "clusters/power_source/power_source.h"
#include "clusters/pressure_measurement/pressure_measurement.h"
#include "clusters/relative_humidity_measurement/relative_humidity_measurement.h"
#include "clusters/rvc_mode/rvc_mode.h"
#include "clusters/rvc_operational_state/rvc_operational_state.h"
#include "clusters/temperature_measurement/temperature_measurement.h"
#include "clusters/thermostat/thermostat.h"

#include "esp_check.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace data_model {
namespace {

const char *const kTag = "fabric_manager_model";

const data_model::cluster_adapter *const kAdapters[] = {
    &data_model::clusters::on_off::adapter,
    &data_model::clusters::level_control::adapter,
    &data_model::clusters::color_control::adapter,
    &data_model::clusters::temperature_measurement::adapter,
    &data_model::clusters::relative_humidity_measurement::adapter,
    &data_model::clusters::occupancy_sensing::adapter,
    &data_model::clusters::boolean_state::adapter,
    &data_model::clusters::power_source::adapter,
    &data_model::clusters::pressure_measurement::adapter,
    &data_model::clusters::illuminance_measurement::adapter,
    &data_model::clusters::thermostat::adapter,
    &data_model::clusters::fan_control::adapter,
    &data_model::clusters::rvc_mode::run_mode_adapter,
    &data_model::clusters::rvc_mode::clean_mode_adapter,
    &data_model::clusters::rvc_operational_state::adapter,
};

const data_model::device *find_identity(const data_model::device_set &set, uint64_t node_id, uint16_t root_endpoint_id)
{
    for (size_t index = 0; index < set.device_count; ++index) {
        const auto &device = set.devices[index];
        if (device.node_id == node_id && device.root_endpoint_id == root_endpoint_id) {
            return &device;
        }
    }
    return nullptr;
}

const data_model::cluster *find_cluster(const data_model::device_set &set, uint64_t node_id, uint16_t endpoint_id,
                                        const data_model::cluster_adapter *adapter)
{
    for (size_t endpoint_index = 0; endpoint_index < set.endpoint_count; ++endpoint_index) {
        const auto &endpoint = set.endpoints[endpoint_index];
        const auto &device = set.devices[endpoint.device_index];
        if (device.node_id != node_id || endpoint.endpoint_id != endpoint_id) {
            continue;
        }
        for (size_t offset = 0; offset < endpoint.cluster_count; ++offset) {
            const auto &cluster = set.clusters[endpoint.first_cluster + offset];
            if (cluster.adapter == adapter)
                return &cluster;
        }
    }
    return nullptr;
}

const data_model::reported_cluster *find_report(const data_model::report &report, uint16_t endpoint_id,
                                                size_t adapter_index)
{
    for (size_t index = 0; index < report.cluster_count; ++index) {
        const auto &cluster = report.clusters[index];
        if (cluster.endpoint_id == endpoint_id && cluster.adapter_index == adapter_index) {
            return &cluster;
        }
    }
    return nullptr;
}

esp_err_t allocate(data_model::device_set &set, size_t device_count, size_t endpoint_count, size_t cluster_count)
{
    set = {};
    set.devices = static_cast<data_model::device *>(calloc(device_count, sizeof(data_model::device)));
    set.endpoints = static_cast<data_model::endpoint *>(calloc(endpoint_count, sizeof(data_model::endpoint)));
    set.clusters = cluster_count ? new (std::nothrow) data_model::cluster[cluster_count]{} : nullptr;
    if ((device_count && !set.devices) || (endpoint_count && !set.endpoints) || (cluster_count && !set.clusters)) {
        data_model::model::free(&set);
        return ESP_ERR_NO_MEM;
    }
    set.device_count = device_count;
    set.endpoint_count = endpoint_count;
    set.cluster_count = cluster_count;
    return ESP_OK;
}

void copy(const data_model::device_set &source, size_t source_device_index, data_model::device_set &target,
          size_t target_device_index, size_t *endpoint_cursor, size_t *cluster_cursor)
{
    const auto &source_device = source.devices[source_device_index];
    auto &target_device = target.devices[target_device_index];
    target_device = source_device;
    target_device.first_endpoint = *endpoint_cursor;
    for (size_t endpoint_offset = 0; endpoint_offset < source_device.endpoint_count; ++endpoint_offset) {
        const auto &source_endpoint = source.endpoints[source_device.first_endpoint + endpoint_offset];
        auto &target_endpoint = target.endpoints[(*endpoint_cursor)++];
        target_endpoint = source_endpoint;
        target_endpoint.device_index = target_device_index;
        target_endpoint.first_cluster = *cluster_cursor;
        for (size_t cluster_offset = 0; cluster_offset < source_endpoint.cluster_count; ++cluster_offset) {
            auto &target_cluster = target.clusters[(*cluster_cursor)++];
            target_cluster = source.clusters[source_endpoint.first_cluster + cluster_offset];
            target_cluster.endpoint_index = *endpoint_cursor - 1;
        }
    }
}

void count(const data_model::device_set &set, const data_model::device &device, size_t *endpoint_count,
           size_t *cluster_count)
{
    *endpoint_count += device.endpoint_count;
    for (size_t offset = 0; offset < device.endpoint_count; ++offset) {
        *cluster_count += set.endpoints[device.first_endpoint + offset].cluster_count;
    }
}

size_t count_clusters(uint32_t mask)
{
    size_t count = 0;
    for (size_t index = 0; index < data_model::cluster_count(); ++index) {
        if (mask & (UINT32_C(1) << index))
            ++count;
    }
    return count;
}

void count_replacement(const data_model::device_set &current, const data_model::report &report,
                       const uint16_t *endpoint_roots, size_t *retained_count, size_t *endpoint_count,
                       size_t *cluster_count)
{
    for (size_t index = 0; index < current.device_count; ++index) {
        const auto &device = current.devices[index];
        if (device.node_id != report.node_id) {
            ++*retained_count;
            count(current, device, endpoint_count, cluster_count);
        }
    }
    for (size_t index = 0; index < report.endpoint_count; ++index) {
        if (endpoint_roots[index] != 0) {
            ++*endpoint_count;
            *cluster_count += count_clusters(report.endpoints[index].cluster_mask);
        }
    }
}

void copy_retained(const data_model::device_set &current, uint64_t replaced_node_id,
                   data_model::device_set &replacement, size_t *device_cursor, size_t *endpoint_cursor,
                   size_t *cluster_cursor)
{
    for (size_t index = 0; index < current.device_count; ++index) {
        if (current.devices[index].node_id == replaced_node_id)
            continue;
        copy(current, index, replacement, (*device_cursor)++, endpoint_cursor, cluster_cursor);
    }
}

esp_err_t initialize_device(fabric_manager_handle_t manager, const data_model::report &report,
                            const data_model::group &group, size_t first_endpoint, data_model::device *device)
{
    const auto *identity = find_identity(manager->set, report.node_id, group.root_endpoint_id);
    device->node_id = report.node_id;
    device->root_endpoint_id = group.root_endpoint_id;
    device->online = true;
    device->baseline_epoch = ++manager->next_epoch;
    device->first_endpoint = first_endpoint;
    snprintf(device->type, sizeof(device->type), "%s", group.type);
    if (identity) {
        device->id = identity->id;
        snprintf(device->name, sizeof(device->name), "%s", identity->name);
        snprintf(device->room, sizeof(device->room), "%s", identity->room);
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(manager->next_device_id != 0 && manager->next_device_id != UINT32_MAX, ESP_ERR_NO_MEM, kTag,
                        "device id exhausted");
    device->id = manager->next_device_id++;
    snprintf(device->name, sizeof(device->name), "%s %" PRIu32, group.type, device->id);
    return ESP_OK;
}

void append_endpoint_clusters(const data_model::report &report, const data_model::reported_endpoint &reported_endpoint,
                              data_model::device_set &replacement, size_t endpoint_cursor, size_t *cluster_cursor)
{
    auto &endpoint = replacement.endpoints[endpoint_cursor];
    endpoint.first_cluster = *cluster_cursor;
    for (size_t adapter_index = 0; adapter_index < data_model::cluster_count(); ++adapter_index) {
        if (!(reported_endpoint.cluster_mask & (UINT32_C(1) << adapter_index))) {
            continue;
        }
        auto &cluster = replacement.clusters[(*cluster_cursor)++];
        cluster.endpoint_index = endpoint_cursor;
        cluster.adapter = data_model::cluster_at(adapter_index);
        const auto *reported = find_report(report, endpoint.endpoint_id, adapter_index);
        if (reported)
            cluster.state = reported->state;
        ++endpoint.cluster_count;
    }
}

esp_err_t append_group(fabric_manager_handle_t manager, const data_model::report &report,
                       const data_model::group &group, const uint16_t *endpoint_roots,
                       data_model::device_set &replacement, size_t device_index, size_t *endpoint_cursor,
                       size_t *cluster_cursor)
{
    auto &device = replacement.devices[device_index];
    ESP_RETURN_ON_ERROR(initialize_device(manager, report, group, *endpoint_cursor, &device), kTag,
                        "initialize device");
    for (size_t index = 0; index < report.endpoint_count; ++index) {
        if (endpoint_roots[index] != group.root_endpoint_id)
            continue;
        const auto &reported_endpoint = report.endpoints[index];
        auto &endpoint = replacement.endpoints[*endpoint_cursor];
        endpoint.device_index = device_index;
        endpoint.endpoint_id = reported_endpoint.endpoint_id;
        const char *name = reported_endpoint.tag_name;
        if (!reported_endpoint.tag_list_reported) {
            for (size_t old_index = 0; old_index < manager->set.endpoint_count; ++old_index) {
                const auto &old = manager->set.endpoints[old_index];
                if (old.endpoint_id == endpoint.endpoint_id &&
                    manager->set.devices[old.device_index].node_id == report.node_id) {
                    name = old.name;
                    break;
                }
            }
        }
        if (name[0]) {
            snprintf(endpoint.name, sizeof(endpoint.name), "%s", name);
        } else {
            snprintf(endpoint.name, sizeof(endpoint.name), "Endpoint %u", endpoint.endpoint_id);
        }
        append_endpoint_clusters(report, reported_endpoint, replacement, *endpoint_cursor, cluster_cursor);
        ++device.endpoint_count;
        ++*endpoint_cursor;
    }
    return ESP_OK;
}

} // namespace
} // namespace data_model

const data_model::cluster_adapter *data_model::cluster_at(size_t index)
{
    return index < sizeof(kAdapters) / sizeof(kAdapters[0]) ? kAdapters[index] : nullptr;
}

size_t data_model::cluster_count(void)
{
    return sizeof(kAdapters) / sizeof(kAdapters[0]);
}

const data_model::cluster_adapter *data_model::find_cluster(uint32_t cluster_id)
{
    for (const auto *adapter : kAdapters) {
        if (adapter->cluster_id == cluster_id)
            return adapter;
    }
    return nullptr;
}

esp_err_t data_model::model::replace_node(fabric_manager_handle_t manager, const data_model::report &report,
                                          data_model::device_set *out_set)
{
    ESP_RETURN_ON_FALSE(manager && out_set, ESP_ERR_INVALID_ARG, kTag, "invalid replacement");
    data_model::group groups[CONFIG_FABRIC_MANAGER_MAX_DEVICES] = {};
    uint16_t endpoint_roots[CONFIG_FABRIC_MANAGER_MAX_ENDPOINTS] = {};
    size_t group_count = 0;
    ESP_RETURN_ON_ERROR(data_model::topology::resolve(report, groups, &group_count, endpoint_roots), kTag,
                        "resolve topology");

    size_t retained_count = 0;
    size_t endpoint_count = 0;
    size_t cluster_count = 0;
    count_replacement(manager->set, report, endpoint_roots, &retained_count, &endpoint_count, &cluster_count);

    data_model::device_set replacement = {};
    ESP_RETURN_ON_ERROR(allocate(replacement, retained_count + group_count, endpoint_count, cluster_count), kTag,
                        "allocate replacement");
    size_t device_cursor = 0;
    size_t endpoint_cursor = 0;
    size_t cluster_cursor = 0;
    copy_retained(manager->set, report.node_id, replacement, &device_cursor, &endpoint_cursor, &cluster_cursor);

    for (size_t index = 0; index < group_count; ++index) {
        esp_err_t error = append_group(manager, report, groups[index], endpoint_roots, replacement, device_cursor++,
                                       &endpoint_cursor, &cluster_cursor);
        if (error != ESP_OK) {
            data_model::model::free(&replacement);
            return error;
        }
    }
    *out_set = replacement;
    return ESP_OK;
}

esp_err_t data_model::model::apply(fabric_manager_handle_t manager, const data_model::report &report, unsigned *views)
{
    *views = 0;
    for (size_t i = 0; i < report.cluster_count; ++i) {
        const auto &incoming = report.clusters[i];
        if (!incoming.touched)
            continue;
        const auto *adapter = data_model::cluster_at(incoming.adapter_index);
        auto *cluster = const_cast<data_model::cluster *>(
            find_cluster(manager->set, report.node_id, incoming.endpoint_id, adapter));
        if (!cluster)
            continue;
        auto &before = cluster->state;
        const auto &after = incoming.state;
        if (memcmp(before.bytes, after.bytes, sizeof(before.bytes)) == 0 &&
            (before.metadata.get() == after.metadata.get() ||
             cJSON_Compare(before.metadata.get(), after.metadata.get(), true)))
            continue;
        const auto &ep = manager->set.endpoints[cluster->endpoint_index];
        *views |= json::record_public_changes(&manager->activity, manager->set.devices[ep.device_index].id,
                                              ep.endpoint_id, *adapter, before, after);
        before = after; // Commit raw state even if the public value is unchanged or comparison failed.
    }
    for (size_t index = 0; index < report.endpoint_count; ++index) {
        const auto &reported = report.endpoints[index];
        if (!reported.tag_list_reported)
            continue;
        for (size_t offset = 0; offset < manager->set.endpoint_count; ++offset) {
            auto &endpoint = manager->set.endpoints[offset];
            if (endpoint.endpoint_id != reported.endpoint_id ||
                manager->set.devices[endpoint.device_index].node_id != report.node_id) {
                continue;
            }
            char name[FABRIC_MANAGER_ENDPOINT_NAME_MAX];
            if (reported.tag_name[0]) {
                snprintf(name, sizeof(name), "%s", reported.tag_name);
            } else {
                snprintf(name, sizeof(name), "Endpoint %u", endpoint.endpoint_id);
            }
            if (strcmp(endpoint.name, name)) {
                snprintf(endpoint.name, sizeof(endpoint.name), "%s", name);
                *views |= FABRIC_DEVICE_STATE;
            }
            break;
        }
    }
    return ESP_OK;
}

esp_err_t data_model::model::retain_nodes(fabric_manager_handle_t manager, const uint64_t *node_ids, size_t node_count,
                                          data_model::device_set *out_set)
{
    ESP_RETURN_ON_FALSE(manager && (node_ids || node_count == 0) && out_set, ESP_ERR_INVALID_ARG, kTag,
                        "invalid node filter");
    size_t device_count = 0;
    size_t endpoint_count = 0;
    size_t cluster_count = 0;
    for (size_t device_index = 0; device_index < manager->set.device_count; ++device_index) {
        const auto &device = manager->set.devices[device_index];
        bool retained = false;
        for (size_t node_index = 0; node_index < node_count; ++node_index) {
            retained |= node_ids[node_index] == device.node_id;
        }
        if (!retained)
            continue;
        ++device_count;
        count(manager->set, device, &endpoint_count, &cluster_count);
    }
    data_model::device_set filtered = {};
    ESP_RETURN_ON_ERROR(allocate(filtered, device_count, endpoint_count, cluster_count), kTag, "allocate node filter");
    size_t device_cursor = 0;
    size_t endpoint_cursor = 0;
    size_t cluster_cursor = 0;
    for (size_t source_index = 0; source_index < manager->set.device_count; ++source_index) {
        const auto &device = manager->set.devices[source_index];
        bool retained = false;
        for (size_t node_index = 0; node_index < node_count; ++node_index) {
            retained |= node_ids[node_index] == device.node_id;
        }
        if (retained) {
            copy(manager->set, source_index, filtered, device_cursor++, &endpoint_cursor, &cluster_cursor);
        }
    }
    *out_set = filtered;
    return ESP_OK;
}

void data_model::model::free(data_model::device_set *set)
{
    if (!set)
        return;
    delete[] set->clusters;
    ::free(set->endpoints);
    ::free(set->devices);
    *set = {};
}
