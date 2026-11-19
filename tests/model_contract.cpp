// Run on esp-emu with the real IDF, CHIP TLV decoder, adapters and manager.
#include "esp_check.h"
#include "esp_memory_utils.h"
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
        assert(writer.EndContainer(outer) == CHIP_NO_ERROR);
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
    data_model::report_begin(nullptr, node, manager);
    if (initial) {
        emit(manager, 0x001d, 1, true, 0); // Descriptor.ServerList
        emit(manager, 0x45, 0, false, 0);
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
    fabric_manager manager = {};
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
    assert(observed.views == FABRIC_DEVICE_STATE);
    observed = {};
    ESP_ERROR_CHECK(fabric_manager_set_device_identity(&manager, id, "Barometer", "Office"));
    assert(observed.views == FABRIC_DEVICE_METADATA);
    observed = {};
    ESP_ERROR_CHECK(fabric_manager_set_device_identity(&manager, id, "Barometer", "Office"));
    assert(!changes && !observed.views);
    update(&manager, 998, true);
    assert(observed.views == FABRIC_DEVICE_STATE);
    fabric_manager_remove_signal(&manager, signal, &observed);
#if CONFIG_FABRIC_AUTOMATION
    auto fired = xSemaphoreCreateBinary();
    assert(fired);
    static const fabric_rule_arg_t fields[] = {{"fail", FABRIC_ARG_BOOLEAN}};
    auto enqueue = [](void *ctx, const char *args) -> esp_err_t {
        if (strstr(args, "\"fail\":true"))
            return ESP_ERR_TIMEOUT;
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(ctx));
        return ESP_OK;
    };
    const fabric_rule_action_t actions[] = {{"record", "Record an event", nullptr, 0, enqueue, fired},
                                            {"try_record", "Test queue rejection", fields, 1, enqueue, fired}};
    fabric_automation_t automation;
    const fabric_rule_action_t duplicate[] = {actions[0], actions[0]};
    assert(fabric_automation_init(&manager, duplicate, 2, &automation) == ESP_ERR_INVALID_ARG);
    ESP_ERROR_CHECK(fabric_automation_init(&manager, actions, 2, &automation));
    char body[768], why[120];
    snprintf(
        body, sizeof(body),
        R"rule({"mode":"raw","enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"boolean_state","property":"state"},"source":"if event.current and not event.previous and matter.get_property(%lu,1,'pressure_measurement','pressure_hpa') < 1000 then automation.invoke_local('record',{}) end"})rule",
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
    data_model::report_begin(nullptr, node, &manager);
    emit(&manager, 0x45, 0, false, 1);
    data_model::report_end(nullptr, node, ESP_OK, false, &manager);
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(2000)) == pdTRUE);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, nullptr, why, sizeof(why)));
    char typed[880];
    snprintf(
        typed, sizeof(typed),
        R"({"mode":"visual","enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa","type":"number","op":">","value":1000},"conditions":[{"device":%lu,"endpoint":1,"cluster":"boolean_state","property":"state","type":"boolean","op":"true"},{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa","type":"number","op":"<","value":1050}],"action":{"native":"record","arguments":{}}})",
        (unsigned long)id, (unsigned long)id, (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, typed, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(200));
    const auto check = [&](int16_t pressure_value, bool expected) {
        update(&manager, pressure_value);
        assert((xSemaphoreTake(fired, pdMS_TO_TICKS(250)) == pdTRUE) == expected);
    };
    check(1001, true);
    check(1002, false); // A report above threshold is not another edge.
    check(999, false);
    check(1080, false); // Second condition blocks this crossing.
    check(1040, false); // Making the condition true does not trigger the rule.
    char *operand = strstr(typed, "1000");
    assert(operand);
    memcpy(operand, "0.25", 4);
    assert(fabric_automation_save(automation, 1, typed, why, sizeof(why)) == ESP_ERR_INVALID_ARG);
    check(990, false);
    check(1040, true); // Bad edit did not replace the rule.
    snprintf(
        body, sizeof(body),
        R"rule({"mode":"raw","enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"for i=1,9 do automation.invoke_local('record',{}) end"})rule",
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
        R"rule({"mode":"raw","enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"automation.invoke_local('record',{}); automation.delay(1); automation.invoke_local('record',{})"})rule",
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
        R"rule({"mode":"raw","enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"automation.delay(60); automation.invoke_local('record',{})"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 2, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(50));
    check(1064, false);
    snprintf(
        body, sizeof(body),
        R"rule({"mode":"raw","enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"automation.invoke_local('try_record',{fail=event.current==1065});automation.invoke_local('record',{})"})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(50));
    check(1065, false);
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
        R"rule({"mode":"raw","enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},"source":"local t=automation.local_time(); if t==nil then return 'unsynced' end; if t.weekday==5 and t.hour==19 and t.minute==45 then automation.invoke_local('record',{}) end"})rule",
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
        R"rule({"mode":"visual","enabled":true,"when":[{"name":"pressure","device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa","type":"number","op":">","value":1000},{"name":"clock","clock":"minute","weekday":5,"start":1200,"end":1320}],"action":{"native":"record","arguments":{}}})rule",
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
        R"rule({"mode":"visual","enabled":true,"when":[{"name":"p","device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa","type":"number","op":">","value":1000},{"name":"clock","clock":"minute","weekdays":[2,5],"at":1202}],"action":{"native":"record","arguments":{}}})rule",
        (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(180)) == pdFALSE);
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(1800)) == pdTRUE);
    check(999, false);
    check(1013, false);
    // The same time window used under If is read only on a property trigger.
    auto clock_guard = [&](const char *days) {
        snprintf(
            body, sizeof(body),
            R"rule({"mode":"visual","enabled":true,"when":{"device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa","type":"number","op":"changes"},"conditions":[{"clock":"minute","start":1202,"end":1203,"weekdays":%s}],"action":{"native":"record","arguments":{}}})rule",
            (unsigned long)id, days);
        return fabric_automation_save(automation, 1, body, why, sizeof(why));
    };
    ESP_ERROR_CHECK(clock_guard("[2,3]"));
    vTaskDelay(pdMS_TO_TICKS(80));
    check(999, false);
    assert(clock_guard("[]") == ESP_ERR_INVALID_ARG);
    assert(clock_guard("[2,2]") == ESP_ERR_INVALID_ARG);
    ESP_ERROR_CHECK(clock_guard("[2,5]"));
    vTaskDelay(pdMS_TO_TICKS(80));
    check(1013, true);
    // Two properties in one committed report must not expose an intermediate combination.
    snprintf(
        body, sizeof(body),
        R"rule({"mode":"raw","enabled":true,"when":[{"name":"pressure","device":%lu,"endpoint":1,"cluster":"pressure_measurement","property":"pressure_hpa"},{"name":"open","device":%lu,"endpoint":1,"cluster":"boolean_state","property":"state"}],"source":"local p,c=event.previous,event.current; if p.open and p.pressure==1013 and not c.open and c.pressure==901 then automation.invoke_local('record',{}) end"})rule",
        (unsigned long)id, (unsigned long)id);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, body, why, sizeof(why)));
    vTaskDelay(pdMS_TO_TICKS(100));
    data_model::report_begin(nullptr, node, &manager);
    emit(&manager, 0x403, 0, false, 901);
    emit(&manager, 0x45, 0, false, 0);
    data_model::report_end(nullptr, node, ESP_OK, false, &manager);
    assert(xSemaphoreTake(fired, pdMS_TO_TICKS(500)) == pdTRUE);
    ESP_ERROR_CHECK(fabric_automation_save(automation, 1, nullptr, why, sizeof(why)));
    fabric_automation_close(automation);
    vSemaphoreDelete(fired);
    ESP_LOGI("contract", "PASS: contact edge runs action without HTTP");
#endif
    ESP_LOGI("contract", "PASS: pressure, atomic discard, scalar reads and automation");
}
