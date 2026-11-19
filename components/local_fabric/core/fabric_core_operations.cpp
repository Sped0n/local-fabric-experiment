/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fabric_core_private.h"

#include "esp_matter.h"
#include "esp_matter_client.h"
#include "esp_matter_controller_client.h"
#include "json_to_tlv.h"

#include <app/ReadClient.h>
#include <app/WriteClient.h>
#include <controller/CommissioneeDeviceProxy.h>
#include <lib/core/TLV.h>
#include <platform/CHIPDeviceLayer.h>

#include <cstdlib>
#include <cstring>

namespace {

namespace client = esp_matter::client;

class fabric_core_operation {
public:
    fabric_core_operation(fabric_core_handle_t core, uint64_t node, fabric_core_operation_t kind,
                          fabric_core_request_completion_cb_t cb, void *ctx)
        : core_(core)
        , node_id_(node)
        , completion_cb_(cb)
        , user_ctx_(ctx)
        , operation_(kind)
    {
    }
    virtual ~fabric_core_operation() = default;
    esp_err_t mark_pending() { return fabric_core_request_begin(core_); }
    void abandon()
    {
        fabric_core_request_end(core_);
        delete this;
    }

protected:
    chip::ScopedNodeId target() const
    {
        return chip::ScopedNodeId(
            node_id_,
            esp_matter::controller::matter_controller_client::get_instance().get_commissioner()->GetFabricIndex());
    }
    void finish(esp_err_t error, const fabric_core_value_t *value = nullptr)
    {
        fabric_core_request_result_t result = {operation_, error, node_id_, {}};
        if (value)
            result.value = *value;
        fabric_core_handle_t core = core_;
        if (completion_cb_)
            completion_cb_(core, &result, user_ctx_);
        delete this;
        fabric_core_request_end(core);
    }
    fabric_core_handle_t core_;
    uint64_t node_id_;

private:
    fabric_core_request_completion_cb_t completion_cb_;
    void *user_ctx_;
    fabric_core_operation_t operation_;
};

class read_operation final : public fabric_core_operation, public client::read_command::delegate {
public:
    using fabric_core_operation::fabric_core_operation;
    CHIP_ERROR start(const fabric_core_attribute_path_t &path)
    {
        chip::app::AttributePathParams attr(path.endpoint.endpoint_id, path.cluster_id, path.attribute_id);
        return interaction.start(target(), client::read_command::request_t({&attr, 1}), *this);
    }
    void OnAttribute(const client::target_t &, const chip::app::ConcreteDataAttributePath &, chip::TLV::TLVReader *data,
                     const chip::app::StatusIB &status) override
    {
        result = fabric_core_from_chip_error(status.ToChipError());
        if (result != ESP_OK || !data)
            return;
        chip::TLV::TLVReader copy = *data;
        result = fabric_core_from_chip_error(fabric_core_decode_tlv_value(copy, value));
    }
    void OnDone(const client::target_t &, CHIP_ERROR err) override
    {
        if (err != CHIP_NO_ERROR)
            result = fabric_core_from_chip_error(err);
        finish(result, result == ESP_OK ? &value : nullptr);
    }

private:
    client::read_command::interaction interaction;
    esp_err_t result = ESP_ERR_NOT_FOUND;
    fabric_core_value_t value = {};
};

class value_encodable final : public chip::app::DataModel::EncodableToTLV {
public:
    explicit value_encodable(const fabric_core_value_t &value)
        : value(value)
    {
    }

    CHIP_ERROR EncodeTo(chip::TLV::TLVWriter &writer, chip::TLV::Tag tag) const override
    {
        if (value.type == FABRIC_CORE_VALUE_JSON) {
            return client::json_encodable(value.text).EncodeTo(writer, tag);
        }

        return encode_scalar(writer, tag);
    }

private:
    CHIP_ERROR encode_octets(chip::TLV::TLVWriter &writer, chip::TLV::Tag tag) const
    {
        const size_t text_length = strnlen(value.text, sizeof(value.text));
        VerifyOrReturnError((text_length & 1U) == 0, CHIP_ERROR_INVALID_ARGUMENT);
        uint8_t bytes[FABRIC_CORE_TEXT_MAX / 2] = {};
        for (size_t index = 0; index < text_length / 2; ++index) {
            const int high = hex_digit(value.text[index * 2]);
            const int low = hex_digit(value.text[index * 2 + 1]);
            VerifyOrReturnError(high >= 0 && low >= 0, CHIP_ERROR_INVALID_ARGUMENT);
            bytes[index] = static_cast<uint8_t>((high << 4) | low);
        }
        return writer.PutBytes(tag, bytes, static_cast<uint32_t>(text_length / 2));
    }

    CHIP_ERROR encode_scalar(chip::TLV::TLVWriter &writer, chip::TLV::Tag tag) const
    {
        switch (value.type) {
        case FABRIC_CORE_VALUE_NULL:
            return writer.PutNull(tag);
        case FABRIC_CORE_VALUE_BOOL:
            return writer.Put(tag, value.data.boolean);
        case FABRIC_CORE_VALUE_SIGNED:
            return writer.Put(tag, value.data.signed_value);
        case FABRIC_CORE_VALUE_UNSIGNED:
            return writer.Put(tag, value.data.unsigned_value);
        case FABRIC_CORE_VALUE_FLOAT:
            return writer.Put(tag, value.data.float_value);
        case FABRIC_CORE_VALUE_STRING:
            return writer.PutString(tag, value.text);
        case FABRIC_CORE_VALUE_OCTETS:
            return encode_octets(writer, tag);
        case FABRIC_CORE_VALUE_JSON:
            return CHIP_ERROR_INCORRECT_STATE;
        default:
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
    }

    static int hex_digit(char digit)
    {
        if (digit >= '0' && digit <= '9') {
            return digit - '0';
        }
        if (digit >= 'a' && digit <= 'f') {
            return digit - 'a' + 10;
        }
        if (digit >= 'A' && digit <= 'F') {
            return digit - 'A' + 10;
        }
        return -1;
    }

    fabric_core_value_t value;
};

class write_operation final : public fabric_core_operation, public client::write_command::delegate {
public:
    using fabric_core_operation::fabric_core_operation;
    CHIP_ERROR start(const fabric_core_attribute_path_t &p, const fabric_core_value_t &v)
    {
        value_encodable encoder(v);
        client::write_command::attribute_write_t attr = {{p.endpoint.endpoint_id, p.cluster_id, p.attribute_id},
                                                         &encoder};
        return interaction.start(target(), {{&attr, 1}, {}}, this);
    }
    void OnResponse(const client::target_t &, const chip::app::ConcreteDataAttributePath &,
                    const chip::app::StatusIB &status) override
    {
        result = status.ToChipError();
    }
    void OnDone(const client::target_t &, CHIP_ERROR err) override
    {
        finish(fabric_core_from_chip_error(err == CHIP_NO_ERROR ? result : err));
    }

private:
    CHIP_ERROR result = CHIP_ERROR_INCORRECT_STATE;
    client::write_command::interaction interaction;
};

class invoke_operation final : public fabric_core_operation, public client::invoke_command::delegate {
public:
    using fabric_core_operation::fabric_core_operation;
    CHIP_ERROR start(const fabric_core_command_path_t &p, const char *args)
    {
        char *wrapped = nullptr;
        if (asprintf(&wrapped, "{\"0:OBJ\":%s}", args) < 0)
            return CHIP_ERROR_NO_MEMORY;
        client::json_encodable fields(wrapped);
        client::invoke_command::request_t request = {p.endpoint.endpoint_id, p.cluster_id, p.command_id, &fields, {}};
        CHIP_ERROR err = interaction.start(target(), request, this);
        free(wrapped);
        return err;
    }
    void OnResponse(const client::target_t &, const chip::app::ConcreteCommandPath &, const chip::app::StatusIB &status,
                    chip::TLV::TLVReader *) override
    {
        result = status.ToChipError();
    }
    void OnDone(const client::target_t &, CHIP_ERROR err) override
    {
        finish(fabric_core_from_chip_error(err == CHIP_NO_ERROR ? result : err));
    }

private:
    CHIP_ERROR result = CHIP_NO_ERROR;
    client::invoke_command::interaction interaction;
};

class remove_operation final : public fabric_core_operation {
public:
    remove_operation(fabric_core_handle_t core, uint64_t node_id, bool force,
                     fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
        : fabric_core_operation(core, node_id, FABRIC_CORE_OPERATION_REMOVE_NODE, completion_cb, user_ctx)
        , force(force)
    {
    }

    void removed(CHIP_ERROR chip_error)
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        esp_err_t error = fabric_core_from_chip_error(chip_error);
        if (error == ESP_OK || force) {
            fabric_core_subscriptions_stop(core_);
            size_t index = 0;
            while (index < core_->node_count && core_->nodes[index].node_id != node_id_) {
                ++index;
            }
            if (index == core_->node_count) {
                error = ESP_ERR_NOT_FOUND;
            } else {
                for (size_t move = index + 1; move < core_->node_count; ++move) {
                    core_->nodes[move - 1] = core_->nodes[move];
                }
                memset(&core_->nodes[core_->node_count - 1], 0, sizeof(core_->nodes[0]));
                --core_->node_count;
            }
            if (error == ESP_OK || force) {
                esp_err_t store_error = fabric_core_store_nodes(core_);
                if (store_error != ESP_OK) {
                    error = store_error;
                } else {
                    fabric_core_private_notify_node_set_changed(core_);
                    if (force) {
                        error = ESP_OK;
                    }
                }
            }
            bool restart = core_->lifecycle == FABRIC_CORE_RUNNING;
            if (restart) {
                esp_err_t restart_error = fabric_core_subscriptions_start(core_);
                if (error == ESP_OK) {
                    error = restart_error;
                }
            }
        }
        finish(error);
    }

private:
    bool force;
};

remove_operation *s_remove_operation;

void fabric_core_remove_complete(chip::NodeId node_id, CHIP_ERROR error)
{
    (void)node_id;
    remove_operation *operation = s_remove_operation;
    s_remove_operation = nullptr;
    if (operation != nullptr) {
        operation->removed(error);
    }
}

void fabric_core_force_remove_work(intptr_t context)
{
    auto *operation = reinterpret_cast<remove_operation *>(context);
    s_remove_operation = nullptr;
    operation->removed(CHIP_ERROR_INTERNAL);
}

template <typename Operation, typename Path, typename... Args>
esp_err_t submit(fabric_core_handle_t core, const Path *path, fabric_core_operation_t kind,
                 fabric_core_request_completion_cb_t cb, void *ctx, Args &&...args)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (!fabric_core_find_node(core, path->endpoint.node_id))
        return ESP_ERR_NOT_FOUND;
    auto *op = new (std::nothrow) Operation(core, path->endpoint.node_id, kind, cb, ctx);
    if (!op)
        return ESP_ERR_NO_MEM;
    esp_err_t err = op->mark_pending();
    if (err != ESP_OK) {
        delete op;
        return err;
    }
    err = fabric_core_from_chip_error(op->start(*path, args...));
    if (err != ESP_OK)
        op->abandon();
    return err;
}
} // namespace

esp_err_t fabric_core_chip_read_attribute(fabric_core_handle_t core, const fabric_core_attribute_path_t *path,
                                          fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    return submit<read_operation>(core, path, FABRIC_CORE_OPERATION_READ_ATTRIBUTE, completion_cb, user_ctx);
}

esp_err_t fabric_core_chip_write_attribute(fabric_core_handle_t core, const fabric_core_attribute_path_t *path,
                                           const fabric_core_value_t *value,
                                           fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    return submit<write_operation>(core, path, FABRIC_CORE_OPERATION_WRITE_ATTRIBUTE, completion_cb, user_ctx, *value);
}

esp_err_t fabric_core_chip_invoke_command(fabric_core_handle_t core, const fabric_core_command_path_t *path,
                                          const char *arguments_json, fabric_core_request_completion_cb_t completion_cb,
                                          void *user_ctx)
{
    return submit<invoke_operation>(core, path, FABRIC_CORE_OPERATION_INVOKE_COMMAND, completion_cb, user_ctx,
                                    arguments_json);
}

esp_err_t fabric_core_chip_remove_node(fabric_core_handle_t core, uint64_t node_id, bool force,
                                       fabric_core_request_completion_cb_t completion_cb, void *user_ctx)
{
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (fabric_core_find_node(core, node_id) == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_remove_operation != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    auto *operation = new (std::nothrow) remove_operation(core, node_id, force, completion_cb, user_ctx);
    if (operation == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t pending_error = operation->mark_pending();
    if (pending_error != ESP_OK) {
        delete operation;
        return pending_error;
    }
    s_remove_operation = operation;
    esp_err_t error =
        esp_matter::controller::matter_controller_client::get_instance().unpair(node_id, fabric_core_remove_complete);
    if (error == ESP_OK) {
        return ESP_OK;
    }
    if (!force) {
        s_remove_operation = nullptr;
        operation->abandon();
        return error;
    }
    CHIP_ERROR schedule_error = chip::DeviceLayer::PlatformMgr().ScheduleWork(fabric_core_force_remove_work,
                                                                              reinterpret_cast<intptr_t>(operation));
    if (schedule_error != CHIP_NO_ERROR) {
        s_remove_operation = nullptr;
        operation->abandon();
        return fabric_core_from_chip_error(schedule_error);
    }
    return ESP_OK;
}
