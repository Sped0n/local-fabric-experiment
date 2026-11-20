// Run on esp-emu with the real IDF, CHIP TLV decoder, adapters and manager.
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "fabric_automation.h"
#include "fabric_manager_private.h"
#include "nvs_flash.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include "../examples/demo/main/json_memory.h"

namespace {
constexpr uint64_t node = 41;
constexpr uint16_t endpoint = 1;
constexpr uint32_t pressure = 0x0403;

void emit(fabric_manager *manager, uint32_t cluster, uint32_t attribute, bool list, int16_t value)
{
    uint8_t bytes[32];
    chip::TLV::TLVWriter writer;
    writer.Init(bytes);
    if (list) {
        chip::TLV::TLVType outer;
        assert(writer.StartContainer(chip::TLV::AnonymousTag(), chip::TLV::kTLVType_Array, outer) == CHIP_NO_ERROR);
        assert(writer.Put(chip::TLV::AnonymousTag(), pressure) == CHIP_NO_ERROR);
        assert(writer.Put(chip::TLV::AnonymousTag(), uint32_t(0x45)) == CHIP_NO_ERROR);
        assert(writer.Put(chip::TLV::AnonymousTag(), uint32_t(0x402)) == CHIP_NO_ERROR);
        assert(writer.Put(chip::TLV::AnonymousTag(), uint32_t(0x201)) == CHIP_NO_ERROR);
        assert(writer.EndContainer(outer) == CHIP_NO_ERROR);
    } else if (cluster == 0x201 && attribute == 0x1c) {
        assert(writer.Put(chip::TLV::AnonymousTag(), uint8_t(value)) == CHIP_NO_ERROR);
    } else if (cluster == 0x45) {
        assert(writer.PutBoolean(chip::TLV::AnonymousTag(), value != 0) == CHIP_NO_ERROR);
    } else {
        assert(writer.Put(chip::TLV::AnonymousTag(), value) == CHIP_NO_ERROR);
    }
    assert(writer.Finalize() == CHIP_NO_ERROR);
    chip::TLV::TLVReader reader;
    reader.Init(bytes, writer.GetLengthWritten());
    assert(reader.Next() == CHIP_NO_ERROR);
    fabric_core_private_attribute_report report = {{{node, endpoint}, cluster, attribute}, &reader, CHIP_NO_ERROR};
    data_model::attribute_reported(nullptr, &report, manager);
}
void update(fabric_manager *manager, int16_t value, bool initial = false, esp_err_t outcome = ESP_OK)
{
    data_model::report_begin(nullptr, node, initial, manager);
    if (initial) {
        emit(manager, 0x001d, 1, true, 0); // Descriptor.ServerList
        emit(manager, 0x45, 0, false, 0);
        emit(manager, 0x402, 0, false, 2151);
        emit(manager, 0x201, 0, false, 2151);
        emit(manager, 0x201, 0x12, false, 2000);
        emit(manager, 0x201, 0x1c, false, 4); // heat
        emit(manager, 0x201, 0x15, false, 500);
    }
    emit(manager, pressure, 0, false, value);
    data_model::report_end(nullptr, node, outcome, initial, manager);
}

} // namespace

extern "C" void app_main()
{
    demo_init_json_memory();
    cJSON *probe = cJSON_CreateString("external");
    char *printed = cJSON_PrintUnformatted(probe);
    assert(probe && esp_ptr_external_ram(probe) && esp_ptr_external_ram(probe->valuestring) &&
           esp_ptr_external_ram(printed));
    cJSON_free(printed);
    cJSON_Delete(probe);
    ESP_ERROR_CHECK(nvs_flash_init());
    static fabric_manager manager = {}; // The configurable activity ring must not consume the main task stack.
    manager.state_mutex = xSemaphoreCreateMutex();
    assert(manager.state_mutex);
    manager.next_device_id = 1;
    ESP_ERROR_CHECK(nvs_open("fabric_test", NVS_READWRITE, &manager.nvs));
    update(&manager, 1013, true);
    assert(manager.set.device_count == 1);
    auto id = manager.set.devices[0].id;
    fabric_property_path_t p = {
        .device = id, .endpoint = 1, .cluster = "pressure_measurement", .property = "pressure_hpa"};
    auto value = [&]() {
        cJSON *d = nullptr;
        ESP_ERROR_CHECK(fabric_manager_property(&manager, &p, &d, nullptr));
        auto v = cJSON_GetObjectItemCaseSensitive(d, "value")->valueint;
        cJSON_Delete(d);
        return v;
    };
    assert(value() == 1013);
    struct observed_t {
        unsigned views;
        uint32_t devices;
    } observed = {};
    auto &changes = observed.devices;
    auto signal = [](void *ctx, unsigned views, uint32_t mask) {
        auto *o = static_cast<observed_t *>(ctx);
        o->views |= views;
        o->devices |= mask;
    };
    ESP_ERROR_CHECK(fabric_manager_add_signal(&manager, signal, &observed));
    update(&manager, 900, false, ESP_FAIL);
    assert(!changes && value() == 1013);
    update(&manager, 1013);
    assert(!changes);
    update(&manager, 998);
    assert(changes == (1u << (id % 32)) && value() == 998);
    assert(observed.views == (FABRIC_DEVICE_STATE | FABRIC_DEVICE_ACTIVITY));
    observed = {};
    ESP_ERROR_CHECK(fabric_manager_set_device_identity(&manager, id, "Barometer", "Office"));
    assert(observed.views == FABRIC_DEVICE_METADATA);
    observed = {};
    ESP_ERROR_CHECK(fabric_manager_set_device_identity(&manager, id, "Barometer", "Office"));
    assert(!changes && !observed.views);
    update(&manager, 998, true);
    assert(observed.views == FABRIC_DEVICE_STATE);
    auto sample = [&](uint32_t cluster, uint32_t attr, int16_t v, esp_err_t result = ESP_OK) {
        data_model::report_begin(nullptr, node, false, &manager);
        emit(&manager, cluster, attr, false, v);
        data_model::report_end(nullptr, node, result, false, &manager);
    };
    assert(manager.activity.count == 1); // Pressure changed to 998 before the second snapshot.
    observed = {};
    sample(0x402, 0, 2152); // Native hundredths differ, public tenths do not.
    sample(0x45, 0, true, ESP_FAIL);
    sample(0x45, 0, false);
    assert(manager.activity.count == 1 && !observed.views);
    sample(0x402, 0, 2161);
    assert(manager.activity.count == 2 && observed.views == (FABRIC_DEVICE_STATE | FABRIC_DEVICE_ACTIVITY));
    auto latest = [&]() -> const device_activity_entry_t & {
        return manager.activity
            .entries[(manager.activity.next + DEVICE_ACTIVITY_CAPACITY - 1) % DEVICE_ACTIVITY_CAPACITY];
    };
    assert(latest().value.number == 21.6 && !strcmp(latest().unit, "celsius"));
    observed = {};
    sample(0x201, 0x16, 3000); // Maximum heating setpoint changes only the available command arguments.
    assert(manager.activity.count == 2 && observed.views == FABRIC_DEVICE_STATE);
    sample(0x201, 0x12, 2200);
    assert(manager.activity.count == 3 && !strcmp(latest().property, "heating_setpoint_c"));
    sample(0x201, 0x1c, 0);
    assert(manager.activity.count == 4 && !strcmp(latest().value.text, "off"));
    // Only local temperature was primed; the old heating setpoint must not cross the subscription gap.
    data_model::report_begin(nullptr, node, true, &manager);
    emit(&manager, 0x001d, 1, true, 0);
    emit(&manager, 0x201, 0, false, 1800);
    emit(&manager, pressure, 0, false, 998);
    data_model::report_end(nullptr, node, ESP_OK, true, &manager);
    observed = {};
    sample(0x201, 0x12, 2300);
    sample(0x45, 0, false);
    assert(manager.activity.count == 4 && observed.views == FABRIC_DEVICE_STATE);
    for (int i = 0; i < DEVICE_ACTIVITY_CAPACITY + 2; ++i) sample(0x45, 0, i % 2 == 0);
    char *activity_json = nullptr;
    ESP_ERROR_CHECK(fabric_manager_activity_json(&manager, &activity_json));
    cJSON *rows = cJSON_Parse(activity_json);
    free(activity_json);
    assert(cJSON_GetArraySize(rows) == DEVICE_ACTIVITY_CAPACITY);
    int direction = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows)
    {
        assert(cJSON_GetObjectItem(row, "device")->valueint == id &&
               cJSON_GetObjectItem(row, "endpoint")->valueint == endpoint);
        assert(!strcmp(cJSON_GetObjectItem(row, "cluster")->valuestring, "boolean_state"));
        assert(!strcmp(cJSON_GetObjectItem(row, "property")->valuestring, "state"));
        assert(cJSON_IsTrue(cJSON_GetObjectItem(row, "current")) ==
               ((DEVICE_ACTIVITY_CAPACITY + 1 - direction) % 2 == 0));
        ++direction;
    }
    cJSON_Delete(rows);
    // Measure report commit only: TLV decoding and the report allocation have already completed.
    sample(0x201, 0x1c, 4);
    sample(0x201, 0x15, 500);
    sample(0x201, 0x16, 3000);
    for (uint32_t cluster : {0x402u, 0x201u}) {
        size_t peak = 0;
        int64_t elapsed = 0;
        for (int i = 0; i < 32; i++) {
            data_model::report_begin(nullptr, node, false, &manager);
            emit(&manager, cluster, 0, false, 2000 + (i % 2) * 10);
            ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_start());
            const size_t start_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            const int64_t start_us = esp_timer_get_time();
            data_model::report_end(nullptr, node, ESP_OK, false, &manager);
            elapsed += esp_timer_get_time() - start_us;
            size_t low = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
            if (start_free > low && start_free - low > peak)
                peak = start_free - low;
            ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_stop());
        }
        ESP_LOGI("contract", "cluster 0x%04lx commit: avg %lld us, peak temporary PSRAM %u bytes",
                 (unsigned long)cluster, (long long)(elapsed / 32), (unsigned)peak);
    }
    sample(0x45, 0, false); // Known false baseline for automation.
    ESP_LOGI("contract", "PASS: device activity, baseline, filtering and bounded retention");
    fabric_manager_remove_signal(&manager, signal, &observed);
#if CONFIG_FABRIC_AUTOMATION
    auto fired = xSemaphoreCreateBinary();
    assert(fired);
    static const fabric_automation_arg_t fields[] = {{"fail", FABRIC_AUTOMATION_ARG_BOOLEAN}};
    auto enqueue = [](void *ctx, const char *args) -> esp_err_t {
        if (strstr(args, "\"fail\":true"))
            return ESP_ERR_TIMEOUT;
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(ctx));
        return ESP_OK;
    };
    const fabric_automation_hook_t actions[] = {{"record", "Record an event", nullptr, 0, enqueue, fired},
                                                {"try_record", "Test queue rejection", fields, 1, enqueue, fired}};
    fabric_automation_t automation;
    const fabric_automation_hook_t duplicate[] = {actions[0], actions[0]};
    assert(fabric_automation_init(&manager, duplicate, 2, &automation) == ESP_ERR_INVALID_ARG);
    ESP_ERROR_CHECK(fabric_automation_init(&manager, actions, 2, &automation));
    char body[768], why[120];
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"boolean_state","property":"state"},"source":"if event.current and not event.previous and matter.get_property(%lu,1,'pressure_measurement','pressure_hpa') < 1000 then automation.invoke_local('record',{}) end"})rule",
        (unsigned long)id, (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    fabric_automation_close(automation);
    ESP_ERROR_CHECK(fabric_automation_init(&manager, actions, 2, &automation)); // restored rule
    // Load never executes actions. Invalid replacements leave the live rule intact.
    char *code = strstr(body, "if event.current");
    assert(code);
    code[0] = '!';
    assert(fabric_automation_save(automation, 1, body, why, sizeof(why)) != ESP_OK);
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(100)) == pdFALSE); // baseline does not fire
    data_model::report_begin(nullptr, node, false, &manager);
    emit(&manager, 0x45, 0, false, 1);
    data_model::report_end(nullptr, node, ESP_OK, false, &manager);
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(2000)) == pdTRUE);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, nullptr, why, sizeof(why)));
    char typed[880];
    snprintf(
        typed, sizeof(typed),
        R"({"enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"editor":{"note":"opaque draft"},"source":"if event.previous>1000 or event.current<=1000 then return end; automation.invoke_local('record',{}); if event.current>=1050 then automation.skip('pressure_hpa < 1050',event.current) end"})",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, typed, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(200));
    const auto check = [&](int16_t pressure_value, bool expected) {
        update(&manager, pressure_value);
        bool actual = xSemaphoreTake(fired, pdMS_TO_TICKS(250)) == pdTRUE;
        if (actual != expected)
            ESP_LOGE("contract", "pressure %d: expected hook %d got %d", pressure_value, expected, actual);
        assert(actual == expected);
    };
    check(1001, true);
    check(1002, false); // A report above threshold is not another edge.
    check(999, false);
    check(1080, false); // Second condition blocks this crossing.
    auto expect_terminal = [&](const char *outcome, const char *why) {
        char *json = nullptr;
        ESP_ERROR_CHECK(fabric_automation_runs(automation, 0, &json));
        cJSON *index = cJSON_Parse(json);
        free(json);
        const cJSON *run = cJSON_GetArrayItem(cJSON_GetObjectItem(index, "finished"), 0);
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(run, "outcome")), outcome));
        assert(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(run, "reason")), why));
        cJSON_Delete(index);
    };
    expect_terminal("skipped", "pressure_hpa < 1050: observed 1080");
    check(1040, false); // Making the condition true does not trigger the rule.
    char *invalid = strstr(typed, "if event.previous");
    assert(invalid);
    *invalid = '!';
    assert(fabric_automation_save(automation, 1, typed, why, sizeof(why)) != ESP_OK);
    check(990, false);
    check(1040, true); // Bad edit did not replace the rule.
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"for i=1,9 do automation.invoke_local('record',{}) end"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(200));
    check(1060, false); // Failing after requesting an action must not submit even the first action.
    char *state = nullptr;
    ESP_ERROR_CHECK(fabric_automation_list(automation, &state));
    assert(strstr(state, "at most eight"));
    cJSON_free(state);
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"automation.invoke_local('record',{}); automation.delay(1); automation.invoke_local('record',{})"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(80));
    check(1061, true);
    check(1062, false); // Retrigger during the delay is consumed, not queued.
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(1200)) == pdTRUE);
    check(1063, true);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, nullptr, why, sizeof(why)));
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(1200)) == pdFALSE); // Cancel unsent step
    // A failing first step cancels the next. A sleeping rule in slot 2 cannot stall slot 1.
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"automation.delay(60); automation.invoke_local('record',{})"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 2, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(50));
    check(1064, false);
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"automation.invoke_local('try_record',{fail=event.current==1065});automation.invoke_local('record',{})"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(50));
    check(1065, false);
    expect_terminal("failed", "Step 1: try_record: ESP_ERR_TIMEOUT");
    ESP_ERROR_CHECK(fabric_automation_list(automation, &state));
    assert(strstr(state, "ESP_ERR_TIMEOUT"));
    free(state);
    check(1066, true);
    xSemaphoreTake(fired, pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(fabric_automation_save(automation, 2, nullptr, why, sizeof(why)));
    // Set only the emulator clock. A plausible wall time is not proof of synchronization.
    assert(setenv("TZ", "TST-2", 1) == 0);
    tzset();
    const timeval friday = {.tv_sec = 1795196700, .tv_usec = 0}; // Friday 17:45 UTC, 19:45 board time.
    assert(settimeofday(&friday, nullptr) == 0);
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"local t=automation.local_time(); if t==nil then return 'unsynced' end; if t.weekday==5 and t.hour==19 and t.minute==45 then automation.invoke_local('record',{}) end"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(200));
    check(1070, false);
    ESP_ERROR_CHECK(fabric_automation_list(automation, &state));
    assert(strstr(state, "\"diagnostic\":\"unsynced\""));
    cJSON_free(state);
    fabric_automation_clock_synchronized(automation);
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(250)) == pdFALSE);
    check(1080, true);
    // Friday 19:59:59 local. Only the next shared minute boundary opens the window.
    auto near_boundary = [](time_t sec) {
        timeval t = {.tv_sec = sec, .tv_usec = 120000};
        assert(settimeofday(&t, nullptr) == 0);
    };
    near_boundary(friday.tv_sec + 899);
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":[{"name":"pressure","device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},{"name":"clock","clock":"minute"}],"source":"local function ready(s) return s.pressure>1000 and s.clock.weekday==5 and s.clock.hour>=20 and s.clock.hour<22 end; if ready(event.current) and not ready(event.previous) then automation.invoke_local('record',{}) end"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(150)) == pdFALSE);
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(2000)) == pdTRUE); // no property report
    // Advance within the same minute: resync must re-arm the wait but not fire an action.
    near_boundary(friday.tv_sec + 959);
    fabric_automation_clock_synchronized(automation);
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(1300)) == pdFALSE); // 20:01, still inside
    check(999, false);
    check(1013, true); // the property can also make the joint test become true
    // Friday is the second selected day. "At" fires on minute arrival, not subsequent sensor edges in that minute.
    near_boundary(friday.tv_sec + 1019); // 20:01:59
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":[{"name":"p","device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},{"name":"clock","clock":"minute"}],"source":"local now=event.current.clock; if now.weekday==5 and now.hour*60+now.minute==1202 and event.previous.clock.minute~=now.minute and event.current.p>1000 then automation.invoke_local('record',{}) end"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(180)) == pdFALSE);
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(1800)) == pdTRUE);
    check(999, false);
    check(1009, false);
    // The same time window used under If is read only on a property trigger.
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"local v=automation.local_time(); if v.weekday~=5 or v.hour~=20 or v.minute~=2 then return automation.skip('outside Friday window') end; automation.invoke_local('record',{})"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(80));
    check(1013, true);
    // Two properties in one committed report must not expose an intermediate combination.
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":[{"name":"pressure","device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},{"name":"open","device":%lu,"endpoint":1,"cluster":"boolean_state","property":"state"}],"source":"local p,c=event.previous,event.current; if p.open and p.pressure==1013 and not c.open and c.pressure==901 then automation.invoke_local('record',{}) end"})rule",
        (unsigned long)id, (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(100));
    data_model::report_begin(nullptr, node, false, &manager);
    emit(&manager, 0x403, 0, false, 901);
    emit(&manager, 0x45, 0, false, 0);
    data_model::report_end(nullptr, node, ESP_OK, false, &manager);
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(500)) == pdTRUE);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, nullptr, why, sizeof(why)));
    // Keep one run sleeping while the other slot completes more than the entire history.
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"automation.delay(60)"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 2, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(60));
    static SemaphoreHandle_t notifications;
    notifications = xSemaphoreCreateCounting(255, 0);
    assert(notifications);
    fabric_automation_set_changed_handler(automation, [](bool trace) {
        if (trace)
            xSemaphoreGive(notifications);
    });
    check(902, false);
    assert(!uxSemaphoreGetCount(notifications)); // The delay started, but no run has finished.
    char *json = nullptr;
    ESP_ERROR_CHECK(fabric_automation_runs(automation, 0, &json));
    cJSON *index = cJSON_Parse(json);
    free(json);
    const cJSON *last_run = cJSON_GetArrayItem(cJSON_GetObjectItem(index, "finished"), 0);
    assert(strstr(cJSON_GetObjectItem(last_run, "trigger")->valuestring, "2 inputs changed"));
    uint32_t sleeping = cJSON_GetObjectItem(last_run, "id")->valueint + 1;
    cJSON_Delete(index);
    snprintf(
        body, sizeof(body),
        R"rule({"enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"automation.invoke_local('record',{})"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(60));
    constexpr int capacity = CONFIG_FABRIC_AUTOMATION_ACTIVITY_CAPACITY;
    for (int i = 0; i < capacity + 2; i++) check(930 + i, true);
    ESP_ERROR_CHECK(fabric_automation_runs(automation, 0, &json));
    index = cJSON_Parse(json);
    free(json);
    auto finished = cJSON_GetObjectItem(index, "finished");
    assert(cJSON_GetObjectItem(index, "capacity")->valueint == capacity);
    assert(uxSemaphoreGetCount(notifications) == capacity + 2);
    assert(cJSON_GetArraySize(finished) == capacity);
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(finished, 0), "id")->valueint == sleeping + capacity + 2);
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(finished, capacity - 1), "id")->valueint == sleeping + 3);
    cJSON_Delete(index);
    assert(fabric_automation_runs(automation, sleeping + 1, &json) == ESP_ERR_NOT_FOUND);
    assert(fabric_automation_runs(automation, sleeping, &json) == ESP_ERR_NOT_FOUND);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 2, nullptr, why, sizeof(why)));
    ESP_ERROR_CHECK(fabric_automation_runs(automation, sleeping, &json));
    assert(strstr(json, "cancelled"));
    free(json);
    ESP_ERROR_CHECK(fabric_automation_runs(automation, 0, &json));
    index = cJSON_Parse(json);
    free(json);
    assert(uxSemaphoreGetCount(notifications) == capacity + 3);
    finished = cJSON_GetObjectItem(index, "finished");
    assert(cJSON_GetArraySize(finished) == capacity);
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(finished, 0), "id")->valueint == sleeping);
    cJSON_Delete(index);
    fabric_automation_close(automation);
    ESP_ERROR_CHECK(fabric_automation_init(&manager, actions, 2, &automation));
    ESP_ERROR_CHECK(fabric_automation_runs(automation, 0, &json));
    index = cJSON_Parse(json);
    free(json);
    assert(!cJSON_GetArraySize(cJSON_GetObjectItem(index, "finished")));
    cJSON_Delete(index);
    fabric_automation_close(automation);
    vSemaphoreDelete(notifications);
    vSemaphoreDelete(fired);
    ESP_LOGI("contract", "PASS: contact edge runs action without HTTP");
#endif
    ESP_LOGI("contract", "PASS: pressure, atomic discard, scalar reads and automation");
}
