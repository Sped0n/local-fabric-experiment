#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "fabric_automation.h"
#include "fabric_utf8.h"
#include "hooks.h"
#include "lauxlib.h"
#include "lua.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "runner.h"
#include "trace.h"
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
    uint32_t id;
    char title[65];
    automation_trace_start_t run; // Nonzero id only until the sequence terminates.
    bool named, clock;
    uint32_t devices;
    bool enabled;
    char *definition, *body;
    uint32_t epochs[FABRIC_MANAGER_PROPERTY_BATCH_MAX];
    cJSON *previous;
    char diagnostic[96];
    input_t inputs[];
} rule_t;
struct fabric_automation {
    fabric_manager_handle_t manager;
    const fabric_automation_hook_t *hooks;
    size_t hook_count;
    nvs_handle_t storage;
    rule_t *slots[SLOT_COUNT];
    automation_runner_t *runner;
    automation_trace_ring_t *trace;
    uint32_t next_rule_id;
    TaskHandle_t worker;
    SemaphoreHandle_t mutex, done;
    bool stop, clock_synced;
    void (*changed)(bool trace);
};
typedef struct {
    fabric_automation_t service;
    size_t allocated;
    automation_step_t *first, *last;
    unsigned step_count;
    bool blocked;
    cJSON *scratch;
    const cJSON *before, *current;
    const rule_t *rule;
    char diagnostic[160];
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
static char *action_args(lua_State *L, int index, const fabric_automation_hook_t *local)
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
    char *args = ok && (!local || automation_hook_args_valid(local, obj)) ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    return args;
}
static automation_step_t *append_step(lua_State *L)
{
    invocation_t *c = context(L);
    if (!c || c->step_count >= RULE_MAX_STEPS)
        luaL_error(L, "at most eight action steps");
    automation_step_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    automation_step_t *p = append_step(L);
    bool local = lua_toboolean(L, lua_upvalueindex(1));
    if (local) {
        const char *name = name_arg(L, 1, 48);
        p->hook = automation_hook_find(call->service->hooks, call->service->hook_count, name);
        if (!p->hook)
            return luaL_error(L, "unknown local action");
    } else {
        p->hook = NULL;
        p->device = coordinate(L, 1, 1, UINT32_MAX);
        p->endpoint = coordinate(L, 2, 0, UINT16_MAX);
        strcpy(p->cluster, name_arg(L, 3, sizeof(p->cluster)));
        strcpy(p->name, name_arg(L, 4, sizeof(p->name)));
    }
    p->args = action_args(L, local ? 2 : 5, p->hook);
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
// Terminate the invocation and discard all its queued actions.
static int skip_run(lua_State *L)
{
    invocation_t *c = context(L);
    size_t len;
    const char *reason = luaL_checklstring(L, 1, &len);
    if (!len)
        return luaL_error(L, "expected a skip reason");
    if (lua_gettop(L) > 1) {
        const char *observed = lua_isboolean(L, 2) ? (lua_toboolean(L, 2) ? "true" : "false") : lua_tostring(L, 2);
        lua_pushfstring(L, "%s: observed %s", reason, observed ? observed : "unavailable");
    } else
        lua_settop(L, 1);
    c->blocked = true;
    return lua_error(L);
}
static int run_body(lua_State *L)
{
    invocation_t *c = context(L);
    const char *body = lua_touserdata(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, skip_run);
    lua_setfield(L, -2, "skip");
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
    const char *source = lua_touserdata(L, 1);
    if (luaL_loadbufferx(L, source, strlen(source), "rule", "t") != LUA_OK)
        return lua_error(L);
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
    return rc == LUA_OK || c->blocked;
}
static void finish_trace(fabric_automation_t a, unsigned slot, const char *outcome, const char *reason)
{
    rule_t *r = a->slots[slot];
    automation_trace_append(a->trace, &r->run, r->id, slot, r->title, outcome, reason);
    r->run.id = 0;
    if (a->changed)
        a->changed(true);
}
static void action_done(void *ctx, unsigned slot, esp_err_t err, unsigned position, const automation_step_t *failed)
{
    fabric_automation_t a = ctx;
    char reason[160] = "";
    if (err != ESP_OK) {
        if (failed->hook)
            snprintf(reason, sizeof(reason), "Step %u: %s: %s", position, failed->hook->name, esp_err_to_name(err));
        else
            snprintf(reason, sizeof(reason), "Step %u: #%lu/%u %s.%s: %s", position, (unsigned long)failed->device,
                     failed->endpoint, failed->cluster, failed->name, esp_err_to_name(err));
    }
    finish_trace(a, slot, err == ESP_OK ? "completed" : "failed", reason);
    rule_t *r = a->slots[slot];
    const char *msg = err == ESP_OK ? "completed" : esp_err_to_name(err);
    if (strcmp(msg, r->diagnostic)) {
        fabric_utf8_copy(r->diagnostic, sizeof(r->diagnostic), msg);
        if (a->changed)
            a->changed(false);
    }
}
static void describe_value(char *out, size_t n, const cJSON *v, bool clock)
{
    if (clock && cJSON_IsNumber(v)) {
        time_t t = (time_t)v->valuedouble * 60;
        struct tm local;
        if (localtime_r(&t, &local) && strftime(out, n, "%a %H:%M", &local))
            return;
    }
    if (cJSON_IsString(v))
        fabric_utf8_copy(out, n, v->valuestring);
    else if (cJSON_IsNumber(v))
        snprintf(out, n, "%.8g", v->valuedouble);
    else
        fabric_utf8_copy(out, n, cJSON_IsTrue(v) ? "true" : cJSON_IsFalse(v) ? "false" : "unknown");
}
static void describe_trigger(automation_trace_start_t *run, const invocation_t *c)
{
    const rule_t *r = c->rule;
    unsigned changes = 0;
    char summary[192] = "";
    for (unsigned i = 0; i < r->count; i++) {
        const input_t *in = &r->inputs[i];
        const cJSON *before = r->named ? field(c->before, in->name) : c->before,
                    *after = r->named ? field(c->current, in->name) : c->current;
        if (cJSON_Compare(before, after, true))
            continue;
        if (++changes > 1)
            continue;
        char old[24], now[24];
        describe_value(old, sizeof(old), before, !in->path.device);
        describe_value(now, sizeof(now), after, !in->path.device);
        if (in->path.device)
            snprintf(summary, sizeof(summary), "#%lu/%u %s.%s: %s → %s", (unsigned long)in->path.device,
                     in->path.endpoint, in->path.cluster, in->path.property, old, now);
        else
            snprintf(summary, sizeof(summary), "Local clock: %s", now);
    }
    size_t prefix = changes > 1 ? snprintf(run->trigger, sizeof(run->trigger), "%u inputs changed: ", changes) : 0;
    fabric_utf8_copy(run->trigger + prefix, sizeof(run->trigger) - prefix, summary);
}
static bool dispatch_value(fabric_automation_t a, unsigned slot, rule_t *r, const cJSON *value, const uint32_t *epochs,
                           bool baseline)
{
    bool reset = baseline || memcmp(epochs, r->epochs, r->count * sizeof(*epochs));
    if (!reset && cJSON_Compare(value, r->previous, true))
        return false;
    cJSON *next = value && !cJSON_IsNull(value) ? cJSON_Duplicate(value, true) : NULL;
    bool changed = !reset && r->previous && next;
    bool run = changed && !automation_runner_active(a->runner, slot);
    if (run) {
        invocation_t c = {.service = a, .before = r->previous, .current = next, .rule = r};
        int64_t start = esp_timer_get_time();
        bool ok = vm_call(&c, run_body, r->body);
        // Preserve explicit Lua diagnostics on the rule resource; a no-action return is not a trace.
        if (!c.blocked && c.diagnostic[0] && strncmp(r->diagnostic, c.diagnostic, sizeof(r->diagnostic) - 1)) {
            fabric_utf8_copy(r->diagnostic, sizeof(r->diagnostic), c.diagnostic);
            if (a->changed)
                a->changed(false);
        }
        if (!ok || c.blocked || c.first) {
            ++a->trace->next_id;
            if (!a->trace->next_id)
                ++a->trace->next_id;
            r->run = (automation_trace_start_t){
                .id = a->trace->next_id, .start_us = start, .wall = a->clock_synced ? time(NULL) : 0};
            describe_trigger(&r->run, &c);
            if (!ok || c.blocked) {
                finish_trace(a, slot, ok ? "skipped" : "failed", c.diagnostic);
                automation_steps_free(c.first);
            } else
                automation_runner_start(a->runner, slot, c.first);
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
            for (unsigned i = 0; i < SLOT_COUNT; i++) automation_runner_cancel(a->runner, i);
            automation_runner_advance(a->runner);
            bool pending = false;
            for (unsigned i = 0; i < SLOT_COUNT; i++) pending |= automation_runner_active(a->runner, i);
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
        int64_t deadline = automation_runner_advance(a->runner);
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
static bool valid_alias(const char *s)
{
    const char *first = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_";
    return s && *s && strchr(first, *s) &&
        !s[strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789")];
}
static esp_err_t prepare(fabric_automation_t a, const char *body, bool validate, rule_t **out, char *msg, size_t size)
{
    if (strlen(body) > 3072) {
        snprintf(msg, size, "rule exceeds 3072 bytes");
        return ESP_ERR_INVALID_SIZE;
    }
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
    const char *source = string(def, "source");
    const cJSON *editor = field(def, "editor"), *member;
    if (!source || !*source || !cJSON_IsBool(field(def, "enabled")) || (editor && !cJSON_IsObject(editor)) ||
        (field(def, "title") && !string(def, "title")))
        goto failed;
    cJSON_ArrayForEach(member, def)
    {
        if (strcmp(member->string, "source") && strcmp(member->string, "when") && strcmp(member->string, "editor") &&
            strcmp(member->string, "title") && strcmp(member->string, "enabled"))
            goto failed;
    }
    fabric_utf8_copy(r->title, sizeof(r->title), string(def, "title") ? string(def, "title") : "");
    r->enabled = cJSON_IsTrue(field(def, "enabled"));
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
            if (!named || r->clock || !string(spec, "clock") || strcmp(string(spec, "clock"), "minute") ||
                cJSON_GetArraySize(spec) != 2)
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
        if (cJSON_GetArraySize(spec) != (named ? 5 : 4))
            goto failed;
        if (validate) {
            cJSON *prop = NULL;
            esp_err_t read = fabric_manager_property(a->manager, &in->path, &prop, NULL);
            cJSON_Delete(prop);
            if (read != ESP_OK) {
                snprintf(msg, size, "unknown watched property");
                goto failed;
            }
        }
    }
    invocation_t call = {.service = a};
    if (!vm_call(&call, compile_body, source)) {
        snprintf(msg, size, "%s", call.diagnostic);
        goto failed;
    }
    r->body = heap_caps_malloc(strlen(source) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (r->body)
        strcpy(r->body, source);
    r->definition = cJSON_PrintUnformatted(def);
    if (!r->definition || !r->body) {
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
        unsigned i = slot - 1;
        if (a->slots[i] && a->slots[i]->run.id)
            finish_trace(a, i, "cancelled", body ? "rule replaced" : "rule removed");
        if (r)
            r->id = a->slots[i] ? a->slots[i]->id : ++a->next_rule_id;
        automation_runner_cancel(a->runner, slot - 1);
        destroy(a->slots[slot - 1]);
        a->slots[slot - 1] = r;
        r = NULL;
        if (a->changed)
            a->changed(false);
        wake(a, UINT32_MAX);
    }
    xSemaphoreGive(a->mutex);
    destroy(r);
    return err;
}
esp_err_t fabric_automation_list_actions(fabric_automation_t a, char **out)
{
    return a ? automation_hooks_json(a->hooks, a->hook_count, out) : ESP_ERR_INVALID_ARG;
}
esp_err_t fabric_automation_init(fabric_manager_handle_t manager, const fabric_automation_hook_t *hooks, size_t count,
                                 fabric_automation_t *out)
{
    if (!manager || !out || !automation_hooks_valid(hooks, count))
        return ESP_ERR_INVALID_ARG;
    fabric_automation_t a = calloc(1, sizeof(*a));
    if (!a)
        return ESP_ERR_NO_MEM;
    a->manager = manager;
    a->hooks = hooks;
    a->hook_count = count;
    a->trace = heap_caps_calloc(1, sizeof(*a->trace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    a->runner = automation_runner_create(a->manager, action_done, a);
    a->mutex = xSemaphoreCreateMutex();
    a->done = xSemaphoreCreateBinary();
    esp_err_t err = a->trace && a->runner && a->mutex && a->done ? nvs_flash_init_partition("rules") : ESP_ERR_NO_MEM;
    if (err == ESP_OK)
        err = nvs_open_from_partition("rules", "automation", NVS_READWRITE, &a->storage);
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
        if (err == ESP_OK)
            a->slots[i - 1]->id = ++a->next_rule_id;
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
            ok = cJSON_AddNumberToObject(item, "rule_id", r->id) &&
                cJSON_AddStringToObject(item, "diagnostic", r->diagnostic);
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
esp_err_t fabric_automation_runs(fabric_automation_t a, uint32_t id, char **out)
{
    if (!a || !out)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(a->mutex, portMAX_DELAY);
    esp_err_t err = automation_trace_json(a->trace, id, out);
    xSemaphoreGive(a->mutex);
    return err;
}
void fabric_automation_set_changed_handler(fabric_automation_t a, void (*fn)(bool trace))
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
    automation_runner_delete(a->runner);
    free(a->trace);
    if (a->mutex)
        vSemaphoreDelete(a->mutex);
    if (a->done)
        vSemaphoreDelete(a->done);
    free(a);
}
