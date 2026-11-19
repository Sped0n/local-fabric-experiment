#include "fabric_automation.h"
#include "action_runner.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "local_actions.h"
#include "lua.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "property_test.h"
#include "rule_source.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#define SLOT_COUNT FABRIC_AUTOMATION_SLOTS
#define LUA_BYTES (64 * 1024)
typedef struct {
    fabric_property_path_t path; // device == 0 is the local minute clock
    char name[24];
} input_t;
typedef struct {
    unsigned count;
    bool named, clock;
    uint32_t devices;
    bool enabled, visual;
    char *definition, *body;
    uint32_t epochs[FABRIC_MANAGER_PROPERTY_BATCH_MAX];
    cJSON *previous;
    char diagnostic[96];
    input_t inputs[];
} rule_t;
struct fabric_automation {
    fabric_manager_handle_t manager;
    const fabric_rule_action_t *actions;
    size_t count;
    nvs_handle_t storage;
    rule_t *slots[SLOT_COUNT];
    action_runner_t *runner;
    TaskHandle_t worker;
    SemaphoreHandle_t mutex, done;
    bool stop, clock_synced;
    void (*changed)(void);
};
typedef struct {
    fabric_automation_t service;
    size_t allocated;
    action_step_t *first, *last;
    unsigned step_count;
    cJSON *scratch;
    const cJSON *before, *current, *def;
    const rule_t *rule;
    char *compiled;
    char diagnostic[96];
} invocation_t;
static const cJSON *field(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}
static const char *string(const cJSON *obj, const char *key)
{
    return cJSON_GetStringValue(field(obj, key));
}
static bool valid_name(const char *s, size_t capacity)
{
    return s && *s && strlen(s) < capacity;
}
static bool key(char *out, size_t capacity, const char *s)
{
    if (!valid_name(s, capacity))
        return false;
    strcpy(out, s);
    return true;
}
static bool integer(const cJSON *item, uint32_t min, uint32_t max)
{
    return cJSON_IsNumber(item) && item->valuedouble >= min && item->valuedouble <= max &&
        trunc(item->valuedouble) == item->valuedouble;
}
static bool selector(fabric_property_path_t *out, const cJSON *obj)
{
    const cJSON *device = field(obj, "device"), *ep = field(obj, "endpoint");
    if (!integer(device, 1, UINT32_MAX) || !integer(ep, 0, UINT16_MAX) ||
        !key(out->cluster, sizeof(out->cluster), string(obj, "cluster")) ||
        !key(out->property, sizeof(out->property), string(obj, "property")))
        return false;
    out->device = (uint32_t)device->valuedouble;
    out->endpoint = (uint16_t)ep->valueint;
    return true;
}
static void *vm_alloc(void *ctx, void *ptr, size_t old, size_t size)
{
    invocation_t *r = ctx;
    if (!ptr)
        old = 0;
    if (!size) {
        free(ptr);
        r->allocated -= old;
        return NULL;
    }
    if (size > LUA_BYTES - (r->allocated - old))
        return NULL;
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p)
        r->allocated = r->allocated - old + size;
    return p;
}
static void limit(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    luaL_error(L, "rule instruction budget exceeded");
}
static void push(lua_State *L, const cJSON *v)
{
    if (cJSON_IsBool(v))
        lua_pushboolean(L, cJSON_IsTrue(v));
    else if (cJSON_IsNumber(v))
        lua_pushnumber(L, v->valuedouble);
    else if (cJSON_IsString(v))
        lua_pushstring(L, v->valuestring);
    else
        lua_pushnil(L);
}
static invocation_t *context(lua_State *L)
{
    void *ctx;
    lua_getallocf(L, &ctx);
    return ctx;
}
static uint32_t coordinate(lua_State *L, int index, uint32_t min, uint32_t max)
{
    lua_Integer n = luaL_checkinteger(L, index);
    if (n < 0 || (uint64_t)n < min || (uint64_t)n > max)
        luaL_argerror(L, index, "coordinate out of range");
    return (uint32_t)n;
}
static const char *name_arg(lua_State *L, int index, size_t cap)
{
    size_t n;
    luaL_checktype(L, index, LUA_TSTRING);
    const char *s = lua_tolstring(L, index, &n);
    if (!n || n >= cap || strlen(s) != n)
        luaL_argerror(L, index, "invalid name");
    return s;
}
static fabric_property_path_t lua_selector(lua_State *L)
{
    fabric_property_path_t s = {.device = coordinate(L, 1, 1, UINT32_MAX)};
    s.endpoint = coordinate(L, 2, 0, UINT16_MAX);
    strcpy(s.cluster, name_arg(L, 3, sizeof(s.cluster)));
    strcpy(s.property, name_arg(L, 4, sizeof(s.property)));
    return s;
}
static int get_property(lua_State *L)
{
    invocation_t *call = context(L);
    fabric_property_path_t path = lua_selector(L);
    esp_err_t err = fabric_manager_property(call->service->manager, &path, &call->scratch, NULL);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
        return luaL_error(L, "property read failed");
    push(L, field(call->scratch, "value"));
    cJSON_Delete(call->scratch);
    call->scratch = NULL;
    return 1;
}
static bool clock_ready(fabric_automation_t a)
{
    return a->clock_synced;
} // VM execution is serialized by service mutex.
// Advertised action arguments are flat named scalars. Do not recursively walk
// arbitrary Lua tables or retain pointers to VM-managed strings after the call.
static char *action_args(lua_State *L, int index, const fabric_rule_action_t *local)
{
    luaL_checktype(L, index, LUA_TTABLE);
    cJSON *obj = cJSON_CreateObject();
    bool ok = obj != NULL;
    unsigned count = 0;
    lua_pushnil(L);
    while (ok && lua_next(L, index)) {
        size_t n = 0;
        const char *k = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &n) : NULL;
        ok = k && n && n < 64 && strlen(k) == n && ++count <= 16;
        if (ok) {
            switch (lua_type(L, -1)) {
            case LUA_TBOOLEAN:
                ok = cJSON_AddBoolToObject(obj, k, lua_toboolean(L, -1));
                break;
            case LUA_TNUMBER: {
                double v = lua_tonumber(L, -1);
                ok = isfinite(v) && cJSON_AddNumberToObject(obj, k, v);
                break;
            }
            case LUA_TSTRING: {
                const char *v = lua_tolstring(L, -1, &n);
                ok = n <= 512 && strlen(v) == n && cJSON_AddStringToObject(obj, k, v);
                break;
            }
            default:
                ok = false;
            }
        }
        lua_pop(L, 1);
    }
    char *args = ok && (!local || local_action_accepts(local, obj)) ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    return args;
}
static action_step_t *append_step(lua_State *L)
{
    invocation_t *c = context(L);
    if (!c || c->step_count >= RULE_MAX_STEPS)
        luaL_error(L, "at most eight action steps");
    action_step_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        luaL_error(L, "out of memory");
    if (c->last)
        c->last->next = p;
    else
        c->first = p;
    c->last = p;
    c->step_count++;
    return p;
}
static int delay(lua_State *L)
{
    uint32_t seconds = coordinate(L, 1, 1, RULE_MAX_DELAY_SECONDS);
    append_step(L)->delay_seconds = seconds;
    return 0;
}
static int invoke(lua_State *L)
{
    invocation_t *call = context(L);
    action_step_t *p = append_step(L);
    bool local = lua_toboolean(L, lua_upvalueindex(1));
    if (local) {
        const char *name = name_arg(L, 1, 48);
        p->native = local_action_find(call->service->actions, call->service->count, name);
        if (!p->native)
            return luaL_error(L, "unknown local action");
    } else {
        p->native = NULL;
        p->device = coordinate(L, 1, 1, UINT32_MAX);
        p->endpoint = coordinate(L, 2, 0, UINT16_MAX);
        strcpy(p->cluster, name_arg(L, 3, sizeof(p->cluster)));
        strcpy(p->name, name_arg(L, 4, sizeof(p->name)));
    }
    p->args = action_args(L, local ? 2 : 5, p->native);
    if (!p->args)
        return luaL_error(L, "invalid action arguments");
    return 0;
}
static int push_time(lua_State *L, time_t now)
{
    struct tm t;
    if (now == (time_t)-1 || !localtime_r(&now, &t)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, t.tm_wday ? t.tm_wday : 7);
    lua_setfield(L, -2, "weekday");
    lua_pushinteger(L, t.tm_hour);
    lua_setfield(L, -2, "hour");
    lua_pushinteger(L, t.tm_min);
    lua_setfield(L, -2, "minute");
    return 1;
}

static int local_time(lua_State *L)
{
    return push_time(L, clock_ready(context(L)->service) ? time(NULL) : (time_t)-1);
}
static void push_inputs(lua_State *L, const rule_t *r, const cJSON *values)
{
    if (!r->named) {
        push(L, values);
        return;
    }
    lua_createtable(L, 0, r->count);
    for (unsigned i = 0; i < r->count; i++) {
        const input_t *in = &r->inputs[i];
        const cJSON *v = field(values, in->name);
        if (in->path.device)
            push(L, v);
        else
            push_time(L, (time_t)v->valuedouble * 60);
        lua_setfield(L, -2, in->name);
    }
}
static int run_body(lua_State *L)
{
    invocation_t *c = context(L);
    const char *body = lua_touserdata(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, delay);
    lua_setfield(L, -2, "delay");
    lua_pushcfunction(L, local_time);
    lua_setfield(L, -2, "local_time");
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, invoke, 1);
    lua_setfield(L, -2, "invoke_local");
    lua_setglobal(L, "automation");
    lua_newtable(L);
    lua_pushcfunction(L, get_property);
    lua_setfield(L, -2, "get_property");
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, invoke, 1);
    lua_setfield(L, -2, "invoke_action");
    lua_setglobal(L, "matter");
    lua_newtable(L);
    push_inputs(L, c->rule, c->before);
    lua_setfield(L, -2, "previous");
    push_inputs(L, c->rule, c->current);
    lua_setfield(L, -2, "current");
    lua_setglobal(L, "event");
    if (luaL_loadbufferx(L, body, strlen(body), "rule", "t") != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 1);
    return 1;
}
static int compile_body(lua_State *L)
{
    invocation_t *c = context(L);
    const char *source = string(c->def, "source");
    if (!source) {
        rule_source_push(L, c->def);
        source = lua_tostring(L, -1);
    }
    if (luaL_loadbufferx(L, source, strlen(source), "rule", "t") != LUA_OK)
        return lua_error(L);
    size_t bytes = strlen(source) + 1;
    c->compiled = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c->compiled)
        memcpy(c->compiled, source, bytes);
    if (!c->compiled)
        return luaL_error(L, "out of memory");
    return 0;
}
static bool vm_call(invocation_t *c, lua_CFunction fn, const char *body)
{
    lua_State *L = lua_newstate(vm_alloc, c, esp_random());
    if (!L) {
        snprintf(c->diagnostic, sizeof(c->diagnostic), "out of memory");
        return false;
    }
    lua_sethook(L, limit, LUA_MASKCOUNT, 12000);
    lua_pushcfunction(L, fn);
    lua_pushlightuserdata(L, (void *)body);
    int rc = lua_pcall(L, 1, 1, 0);
    if (lua_type(L, -1) == LUA_TSTRING)
        snprintf(c->diagnostic, sizeof(c->diagnostic), "%s", lua_tostring(L, -1));
    lua_close(L);
    cJSON_Delete(c->scratch);
    return rc == LUA_OK;
}
static void action_done(void *ctx, unsigned slot, esp_err_t err)
{
    fabric_automation_t a = ctx;
    rule_t *r = a->slots[slot];
    if (!r)
        return;
    const char *msg = err == ESP_OK ? "completed" : esp_err_to_name(err);
    if (strcmp(msg, r->diagnostic)) {
        snprintf(r->diagnostic, sizeof(r->diagnostic), "%s", msg);
        if (a->changed)
            a->changed();
    }
}
static bool dispatch_value(fabric_automation_t a, unsigned slot, rule_t *r, const cJSON *value, const uint32_t *epochs,
                           bool baseline)
{
    bool reset = baseline || memcmp(epochs, r->epochs, r->count * sizeof(*epochs));
    if (!reset && cJSON_Compare(value, r->previous, true))
        return false;
    cJSON *next = value && !cJSON_IsNull(value) ? cJSON_Duplicate(value, true) : NULL;
    bool run = !reset && r->previous && next && !action_runner_active(a->runner, slot);
    if (run) {
        invocation_t c = {.service = a, .rule = r, .before = r->previous, .current = next};
        if (vm_call(&c, run_body, r->body) && c.first) {
            action_runner_start(a->runner, slot, c.first);
            snprintf(c.diagnostic, sizeof(c.diagnostic), "running");
        } else
            action_steps_free(c.first);
        if (c.diagnostic[0] && strcmp(c.diagnostic, r->diagnostic)) {
            strcpy(r->diagnostic, c.diagnostic);
            if (a->changed)
                a->changed();
        }
    }
    cJSON_Delete(r->previous);
    r->previous = next;
    memcpy(r->epochs, epochs, r->count * sizeof(*epochs));
    return run;
}
static void wake(void *ctx, uint32_t devices)
{
    fabric_automation_t a = ctx;
    xTaskNotify(a->worker, devices, eSetBits);
}
static void devices_changed(void *ctx, unsigned views, uint32_t devices)
{
    if (views & FABRIC_DEVICE_STATE)
        wake(ctx, devices);
}
static bool same_path(const fabric_property_path_t *a, const fabric_property_path_t *b)
{
    return a->device == b->device && a->endpoint == b->endpoint && !strcmp(a->cluster, b->cluster) &&
        !strcmp(a->property, b->property);
}
// Return all inputs or no baseline. Manager captures the property states in one commit snapshot.
static cJSON *sample(fabric_automation_t a, const rule_t *r, time_t minute, uint32_t *epochs)
{
    const fabric_property_path_t *paths[FABRIC_MANAGER_PROPERTY_BATCH_MAX];
    cJSON *props[FABRIC_MANAGER_PROPERTY_BATCH_MAX] = {0};
    uint32_t batch_epochs[FABRIC_MANAGER_PROPERTY_BATCH_MAX];
    size_t n = 0;
    if (r->clock && minute < 0)
        return NULL;
    for (unsigned i = 0; i < r->count; i++)
        if (r->inputs[i].path.device)
            paths[n++] = &r->inputs[i].path;
    if (n && fabric_manager_properties(a->manager, paths, n, props, batch_epochs) != ESP_OK)
        return NULL;
    cJSON *out = r->named ? cJSON_CreateObject() : NULL;
    size_t j = 0;
    for (unsigned i = 0; i < r->count; i++) {
        const input_t *in = &r->inputs[i];
        cJSON *v;
        if (in->path.device) {
            epochs[i] = batch_epochs[j];
            v = cJSON_DetachItemFromObjectCaseSensitive(props[j++], "value");
        } else
            v = cJSON_CreateNumber(minute);
        if (!r->named) {
            out = v;
            break;
        }
        if (!v || cJSON_IsNull(v) || !cJSON_AddItemToObject(out, in->name, v)) {
            cJSON_Delete(v);
            cJSON_Delete(out);
            out = NULL;
            break;
        }
    }
    for (size_t i = 0; i < n; i++) cJSON_Delete(props[i]);
    return out;
}
static bool same_inputs(const rule_t *a, const rule_t *b)
{
    if (a->count != b->count || a->named != b->named)
        return false;
    for (unsigned i = 0; i < a->count; i++)
        if (!same_path(&a->inputs[i].path, &b->inputs[i].path) || strcmp(a->inputs[i].name, b->inputs[i].name))
            return false;
    return true;
}
static int64_t wall_us(void)
{
    struct timeval tv;
    return gettimeofday(&tv, NULL) == 0 ? (int64_t)tv.tv_sec * 1000000 + tv.tv_usec : -1;
}
#define MINUTE_US INT64_C(60000000)
static void worker(void *ctx)
{
    fabric_automation_t a = ctx;
    TickType_t wait = portMAX_DELAY;
    time_t last = -1;
    for (;;) {
        uint32_t mask = 0;
        xTaskNotifyWait(0, UINT32_MAX, &mask, wait);
        xSemaphoreTake(a->mutex, portMAX_DELAY);
        if (a->stop) {
            for (unsigned i = 0; i < SLOT_COUNT; i++) action_runner_cancel(a->runner, i);
            action_runner_advance(a->runner);
            bool pending = false;
            for (unsigned i = 0; i < SLOT_COUNT; i++) pending |= action_runner_active(a->runner, i);
            xSemaphoreGive(a->mutex);
            if (!pending)
                break;
            wait = portMAX_DELAY;
            continue;
        }
        bool watches_clock = false;
        for (unsigned i = 0; i < SLOT_COUNT; i++) {
            rule_t *r = a->slots[i];
            if (r && r->enabled && r->clock) {
                watches_clock = true;
                break;
            }
        }
        int64_t now = watches_clock && a->clock_synced ? wall_us() : -1;
        time_t minute = now < 0 ? -1 : now / MINUTE_US;
        bool tick = minute != last, reset = tick && (last < 0 || minute != last + 1);
#if CONFIG_FABRIC_AUTOMATION_TIMING
        int64_t start = esp_timer_get_time(), lua_us = 0;
        unsigned executions = 0;
#endif
        uint32_t done = 0;
        for (unsigned i = 0; i < SLOT_COUNT; i++) {
            rule_t *r = a->slots[i];
            if (!r || !r->enabled || (done & (1u << i)) ||
                (!(mask & r->devices) && !(r->clock && (tick || mask == UINT32_MAX))))
                continue;
            uint32_t epochs[FABRIC_MANAGER_PROPERTY_BATCH_MAX] = {0};
            cJSON *values = sample(a, r, minute, epochs);
            for (unsigned j = i; j < SLOT_COUNT; j++) {
                rule_t *other = a->slots[j];
                if (!other || !other->enabled || !same_inputs(r, other))
                    continue;
                done |= 1u << j;
#if CONFIG_FABRIC_AUTOMATION_TIMING
                int64_t t = esp_timer_get_time();
                if (dispatch_value(a, j, other, values, epochs, other->clock && reset)) {
                    executions++;
                    lua_us += esp_timer_get_time() - t;
                }
#else
                dispatch_value(a, j, other, values, epochs, other->clock && reset);
#endif
            }
            cJSON_Delete(values);
        }
        int64_t deadline = action_runner_advance(a->runner);
#if CONFIG_FABRIC_AUTOMATION_TIMING
        if (tick && !reset)
            ESP_LOGI("rule_timing", "minute=%lld calls=%u dispatch_us=%lld batch_us=%lld", (long long)minute,
                     executions, (long long)lua_us, (long long)(esp_timer_get_time() - start));
#endif
        last = minute;
        int64_t left = deadline == INT64_MAX ? INT64_MAX : deadline - esp_timer_get_time();
        if (now >= 0) {
            int64_t wall_left = (minute + 1) * MINUTE_US - wall_us();
            if (wall_left < left)
                left = wall_left;
        }
        wait = left == INT64_MAX ? portMAX_DELAY : left <= 0 ? 0 : pdMS_TO_TICKS((left + 999) / 1000) + 1;
        xSemaphoreGive(a->mutex);
    }
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}
static void destroy(rule_t *r)
{
    if (r) {
        cJSON_Delete(r->previous);
        free(r->body);
        free(r->definition);
        free(r);
    }
}
static bool valid_test(fabric_automation_t a, const cJSON *spec, bool when, bool validate)
{
    fabric_property_path_t p = {0};
    property_test_t test;
    if (!selector(&p, spec) || !property_test_parse(&test, spec, when))
        return false;
    if (!validate)
        return true;
    cJSON *d = NULL;
    bool ok = fabric_manager_property(a->manager, &p, &d, NULL) == ESP_OK && property_test_supported(&test, d);
    cJSON_Delete(d);
    return ok;
}
static bool valid_alias(const char *s)
{
    const char *letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_";
    return strchr(letters, *s) &&
        strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == strlen(s);
}
// Validate visual operands, not the clock predicate. Only generated Lua interprets them.
static bool valid_clock(const cJSON *o, bool visual, bool named)
{
    const char *unit = string(o, "clock");
    if (!unit || strcmp(unit, "minute"))
        return false;
    if (!visual)
        return cJSON_GetArraySize(o) == 2;
    const cJSON *day = field(o, "weekday"), *days = field(o, "weekdays"), *at = field(o, "at"),
                *start = field(o, "start"), *end = field(o, "end");
    if (cJSON_GetArraySize(o) !=
        1 + named + (day != NULL) + (days != NULL) + (at != NULL) + (start != NULL) + (end != NULL))
        return false;
    if (day && (days || !integer(day, 1, 7)))
        return false; // Existing single-day definitions.
    if (days) {
        if (!cJSON_IsArray(days) || cJSON_GetArraySize(days) < 1 || cJSON_GetArraySize(days) > 7)
            return false;
        unsigned seen = 0;
        const cJSON *v;
        cJSON_ArrayForEach(v, days)
        {
            if (!integer(v, 1, 7) || (seen & (1u << v->valueint)))
                return false;
            seen |= 1u << v->valueint;
        }
    }
    if (at)
        return named && !start && !end && integer(at, 0, 1439);
    return (!start && !end) || (integer(start, 0, 1439) && integer(end, 1, 1440) && start->valueint < end->valueint);
}
static bool valid_step(fabric_automation_t a, const cJSON *action, bool validate)
{
    const cJSON *seconds = field(action, "delay_s");
    if (seconds)
        return cJSON_GetArraySize(action) == 1 && integer(seconds, 1, RULE_MAX_DELAY_SECONDS);
    if (!cJSON_IsObject(field(action, "arguments")))
        return false;
    if (field(action, "native")) {
        const char *name = string(action, "native");
        if (!valid_name(name, 48) || cJSON_GetArraySize(action) != 2)
            return false;
        // Keep saved rules editable if an integration is missing after a firmware change.
        return !validate ||
            local_action_accepts(local_action_find(a->actions, a->count, name), field(action, "arguments"));
    } else if (!integer(field(action, "device"), 1, UINT32_MAX) || !integer(field(action, "endpoint"), 0, UINT16_MAX) ||
               !valid_name(string(action, "cluster"), 48) || !valid_name(string(action, "name"), 48))
        return false;
    return true;
}
static esp_err_t prepare(fabric_automation_t a, const char *body, bool validate, rule_t **out, char *msg, size_t size)
{
    if (strlen(body) > 3072)
        return ESP_ERR_INVALID_SIZE;
    cJSON *def = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *when = field(def, "when");
    bool named = cJSON_IsArray(when);
    int count = named ? cJSON_GetArraySize(when) : 1;
    if (count < 1 || count > FABRIC_MANAGER_PROPERTY_BATCH_MAX) {
        cJSON_Delete(def);
        return ESP_ERR_INVALID_ARG;
    }
    rule_t *r = heap_caps_calloc(1, sizeof(*r) + count * sizeof(input_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!r) {
        cJSON_Delete(def);
        return ESP_ERR_NO_MEM;
    }
    r->count = count;
    r->named = named;
    const cJSON *conditions = field(def, "conditions"), *action = field(def, "action");
    const char *mode = string(def, "mode");
    if (!mode || !cJSON_IsBool(field(def, "enabled")) || field(def, "trigger") || field(def, "condition") ||
        field(def, "becomes"))
        goto failed;
    r->enabled = cJSON_IsTrue(field(def, "enabled"));
    r->visual = !strcmp(mode, "visual");
    for (int i = 0; i < count; i++) {
        input_t *in = &r->inputs[i];
        const cJSON *spec = named ? cJSON_GetArrayItem(when, i) : when;
        if (named) {
            const char *name = string(spec, "name");
            if (!key(in->name, sizeof(in->name), name) || !valid_alias(name))
                goto failed;
            for (int j = 0; j < i; j++)
                if (!strcmp(name, r->inputs[j].name))
                    goto failed;
        }
        if (field(spec, "clock")) {
            if (!named || r->clock || !valid_clock(spec, r->visual, true))
                goto failed;
            r->clock = true;
            continue;
        }
        if (!selector(&in->path, spec))
            goto failed;
        r->devices |= 1u << (in->path.device % 32);
        for (int j = 0; j < i; j++)
            if (same_path(&in->path, &r->inputs[j].path))
                goto failed;
        if (r->visual) {
            if (!valid_test(a, spec, !named, validate)) {
                snprintf(msg, size, "trigger does not match property schema");
                goto failed;
            }
        } else {
            if (cJSON_GetArraySize(spec) != (named ? 5 : 4))
                goto failed;
            if (validate) {
                cJSON *prop = NULL;
                esp_err_t read = fabric_manager_property(a->manager, &in->path, &prop, NULL);
                cJSON_Delete(prop);
                if (read != ESP_OK)
                    goto failed;
            }
        }
    }
    if (r->visual) {
        if (field(def, "source"))
            goto failed;
        if ((conditions && !cJSON_IsArray(conditions)) || cJSON_GetArraySize(conditions) > 4)
            goto failed;
        const cJSON *c;
        cJSON_ArrayForEach(c, conditions)
        {
            if (field(c, "clock") ? !valid_clock(c, true, false) : !valid_test(a, c, false, validate))
                goto failed;
        }
        int steps = cJSON_IsArray(action) ? cJSON_GetArraySize(action) : 1;
        if (steps < 1 || steps > RULE_MAX_STEPS)
            goto failed;
        for (int i = 0; i < steps; i++)
            if (!valid_step(a, cJSON_IsArray(action) ? cJSON_GetArrayItem(action, i) : action, validate)) {
                snprintf(msg, size, "invalid action or arguments at step %d", i + 1);
                goto failed;
            }
    } else {
        const char *source = string(def, "source");
        if (strcmp(mode, "raw") || !source || !*source || conditions || action)
            goto failed;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(def, "diagnostic");
    cJSON_DeleteItemFromObjectCaseSensitive(def, "generated");
    invocation_t call = {.service = a, .def = def};
    if (!vm_call(&call, compile_body, NULL)) {
        snprintf(msg, size, "%s", call.diagnostic);
        goto failed;
    }
    r->body = call.compiled;
    r->definition = cJSON_PrintUnformatted(def);
    if (!r->definition) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    *out = r;
    cJSON_Delete(def);
    return ESP_OK;
failed:
    cJSON_Delete(def);
    destroy(r);
    return err;
}
static void key_name(char name[8], unsigned slot)
{
    snprintf(name, 8, "r%u", slot);
}
esp_err_t fabric_automation_save(fabric_automation_t a, unsigned slot, const char *body, char *msg, size_t size)
{
    if (!a || !slot || slot > SLOT_COUNT || !msg || !size)
        return ESP_ERR_INVALID_ARG;
    snprintf(msg, size, "invalid rule");
    rule_t *r = NULL;
    esp_err_t err = body ? prepare(a, body, true, &r, msg, size) : ESP_OK;
    if (err != ESP_OK)
        return err;
    char name[8];
    key_name(name, slot);
    xSemaphoreTake(a->mutex, portMAX_DELAY);
    err = body ? nvs_set_str(a->storage, name, r->definition) : nvs_erase_key(a->storage, name);
    if (!body && err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_commit(a->storage);
    if (err == ESP_OK) {
        action_runner_cancel(a->runner, slot - 1);
        destroy(a->slots[slot - 1]);
        a->slots[slot - 1] = r;
        r = NULL;
        if (a->changed)
            a->changed();
        wake(a, UINT32_MAX);
    }
    xSemaphoreGive(a->mutex);
    destroy(r);
    return err;
}
esp_err_t fabric_automation_list_actions(fabric_automation_t a, char **out)
{
    return a ? local_actions_json(a->actions, a->count, out) : ESP_ERR_INVALID_ARG;
}
esp_err_t fabric_automation_init(fabric_manager_handle_t manager, const fabric_rule_action_t *actions, size_t count,
                                 fabric_automation_t *out)
{
    if (!manager || !out || !local_actions_valid(actions, count))
        return ESP_ERR_INVALID_ARG;
    fabric_automation_t a = calloc(1, sizeof(*a));
    if (!a)
        return ESP_ERR_NO_MEM;
    a->manager = manager;
    a->actions = actions;
    a->count = count;
    a->runner = action_runner_create(a->manager, action_done, a);
    a->mutex = xSemaphoreCreateMutex();
    a->done = xSemaphoreCreateBinary();
    esp_err_t err = a->runner && a->mutex && a->done ? nvs_flash_init_partition("rules") : ESP_ERR_NO_MEM;
    if (err == ESP_OK)
        err = nvs_open_from_partition("rules", "fabric_rules", NVS_READWRITE, &a->storage);
    for (unsigned i = 1; err == ESP_OK && i <= SLOT_COUNT; i++) {
        char name[8];
        key_name(name, i);
        size_t n = 0;
        err = nvs_get_str(a->storage, name, NULL, &n);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
            continue;
        }
        if (err != ESP_OK)
            break;
        char *body = n <= 3073 ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        err = body ? nvs_get_str(a->storage, name, body, &n) : ESP_ERR_NO_MEM;
        char msg[96];
        if (err == ESP_OK)
            err = prepare(a, body, false, &a->slots[i - 1], msg, sizeof(msg));
        free(body);
    }
    if (err == ESP_OK && xTaskCreate(worker, "fabric_rules", 12288, a, 4, &a->worker) != pdPASS)
        err = ESP_ERR_NO_MEM;
    if (err == ESP_OK)
        err = fabric_manager_add_signal(manager, devices_changed, a);
    if (err != ESP_OK) {
        fabric_automation_close(a);
        return err;
    }
    wake(a, UINT32_MAX);
    *out = a;
    return ESP_OK;
}
esp_err_t fabric_automation_list(fabric_automation_t a, char **out)
{
    if (!a || !out)
        return ESP_ERR_INVALID_ARG;
    cJSON *list = cJSON_CreateArray();
    if (!list)
        return ESP_ERR_NO_MEM;
    bool ok = true;
    xSemaphoreTake(a->mutex, portMAX_DELAY);
    for (unsigned i = 0; i < SLOT_COUNT && ok; i++) {
        rule_t *r = a->slots[i];
        cJSON *item = r ? cJSON_Parse(r->definition) : cJSON_CreateNull();
        ok = cJSON_AddItemToArray(list, item);
        if (ok && r)
            ok = cJSON_AddStringToObject(item, "diagnostic", r->diagnostic) &&
                (!r->visual || cJSON_AddStringToObject(item, "generated", r->body));
    }
    xSemaphoreGive(a->mutex);
    *out = ok ? cJSON_PrintUnformatted(list) : NULL;
    cJSON_Delete(list);
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}
void fabric_automation_clock_synchronized(fabric_automation_t a)
{
    xSemaphoreTake(a->mutex, portMAX_DELAY);
    a->clock_synced = true;
    xTaskNotify(a->worker, 0, eNoAction);
    xSemaphoreGive(a->mutex);
}
void fabric_automation_set_changed_handler(fabric_automation_t a, void (*fn)(void))
{
    xSemaphoreTake(a->mutex, portMAX_DELAY);
    a->changed = fn;
    xSemaphoreGive(a->mutex);
}
void fabric_automation_close(fabric_automation_t a)
{
    if (!a)
        return;
    if (a->worker) {
        fabric_manager_remove_signal(a->manager, devices_changed, a);
        xSemaphoreTake(a->mutex, portMAX_DELAY);
        a->stop = true;
        xSemaphoreGive(a->mutex);
        wake(a, UINT32_MAX);
        xSemaphoreTake(a->done, portMAX_DELAY);
    }
    for (unsigned i = 0; i < SLOT_COUNT; i++) destroy(a->slots[i]);
    if (a->storage)
        nvs_close(a->storage);
    action_runner_delete(a->runner);
    if (a->mutex)
        vSemaphoreDelete(a->mutex);
    if (a->done)
        vSemaphoreDelete(a->done);
    free(a);
}
