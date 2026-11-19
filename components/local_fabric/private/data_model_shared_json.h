/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "cJSON.h"

#include <new>
#include <utility>
#include "freertos/FreeRTOS.h"

namespace data_model {

// Immutable variable-length adapter data shared by reports and committed state.
class shared_json {
public:
    shared_json() = default;
    shared_json(const shared_json &other)
        : value_(other.value_)
    {
        if (value_) {
            portENTER_CRITICAL(&lock_);
            ++value_->references;
            portEXIT_CRITICAL(&lock_);
        }
    }
    shared_json(shared_json &&other) noexcept
        : value_(std::exchange(other.value_, nullptr))
    {
    }
    shared_json &operator=(const shared_json &other)
    {
        shared_json replacement(other);
        std::swap(value_, replacement.value_);
        return *this;
    }
    shared_json &operator=(shared_json &&other) noexcept
    {
        shared_json replacement(std::move(other));
        std::swap(value_, replacement.value_);
        return *this;
    }
    ~shared_json() { release(); }

    const cJSON *get() const { return value_ ? value_->json : nullptr; }

    // Takes ownership even on failure; allocation failure preserves this value.
    bool reset(cJSON *json)
    {
        storage *replacement = json ? new (std::nothrow) storage(json) : nullptr;
        if (json && !replacement) {
            cJSON_Delete(json);
            return false;
        }
        release();
        value_ = replacement;
        return true;
    }

private:
    struct storage {
        explicit storage(cJSON *value)
            : json(value)
        {
        }
        size_t references = 1;
        cJSON *json;
    };

    void release()
    {
        if (!value_)
            return;
        portENTER_CRITICAL(&lock_);
        bool last = --value_->references == 0;
        portEXIT_CRITICAL(&lock_);
        if (last) {
            cJSON_Delete(value_->json);
            delete value_;
        }
        value_ = nullptr;
    }

    inline static portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    storage *value_ = nullptr;
};

} // namespace data_model
