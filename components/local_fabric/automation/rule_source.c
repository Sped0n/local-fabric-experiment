#include "rule_source.h"
#include "lauxlib.h"
#include <stdio.h>
#include <string.h>

static const cJSON *field(const cJSON *o, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(o, key);
}
static void quoted(luaL_Buffer *b, const char *s)
{
    luaL_addchar(b, '"');
    for (; *s; s++) {
        unsigned char c = *s;
        if (c < ' ' || c == '"' || c == '\\') {
            char esc[5];
            snprintf(esc, sizeof(esc), "\\%03u", (unsigned)c);
            luaL_addstring(b, esc);
        } else
            luaL_addchar(b, *s);
    }
    luaL_addchar(b, '"');
}
static void literal(luaL_Buffer *b, const cJSON *v)
{
    if (cJSON_IsString(v))
        quoted(b, v->valuestring);
    else if (cJSON_IsBool(v))
        luaL_addstring(b, cJSON_IsTrue(v) ? "true" : "false");
    else if (cJSON_IsNumber(v)) {
        char n[32];
        snprintf(n, sizeof(n), "%.17g", v->valuedouble);
        luaL_addstring(b, n);
    } else
        luaL_error(b->L, "expected scalar operand");
}
static void path(luaL_Buffer *b, const cJSON *p, const char *member)
{
    const char *keys[] = {"device", "endpoint", "cluster", member};
    for (int i = 0; i < 4; i++) {
        if (i)
            luaL_addstring(b, ", ");
        literal(b, field(p, keys[i]));
    }
}
static void comparison(luaL_Buffer *b, const cJSON *test, const char *var)
{
    const char *op = cJSON_GetStringValue(field(test, "op"));
    luaL_addstring(b, var);
    luaL_addchar(b, ' ');
    if (!strcmp(op, "true") || !strcmp(op, "false")) {
        luaL_addstring(b, "== ");
        luaL_addstring(b, op);
    } else {
        luaL_addstring(b, !strcmp(op, "!=") ? "~=" : op);
        luaL_addchar(b, ' ');
        literal(b, field(test, "value"));
    }
}
static void emit_step(luaL_Buffer *b, const cJSON *action)
{
    if (field(action, "delay_s")) {
        luaL_addstring(b, "  automation.delay(");
        literal(b, field(action, "delay_s"));
        luaL_addstring(b, ")\n");
        return;
    }
    if (field(action, "native")) {
        luaL_addstring(b, "  automation.invoke_local(");
        literal(b, field(action, "native"));
    } else {
        luaL_addstring(b, "  matter.invoke_action(");
        path(b, action, "name");
    }
    luaL_addstring(b, ", {");
    const cJSON *v;
    cJSON_ArrayForEach(v, field(action, "arguments"))
    {
        if (v->prev && v != field(action, "arguments")->child)
            luaL_addstring(b, ", ");
        luaL_addchar(b, '[');
        quoted(b, v->string);
        luaL_addstring(b, "]=");
        literal(b, v);
    }
    luaL_addstring(b, "})\n");
}
// Emit a predicate for the local clock table v. Both When and If use the same day/time semantics.
static void clock_predicate(luaL_Buffer *b, const cJSON *test)
{
    const cJSON *day = field(test, "weekday"), *days = field(test, "weekdays"), *at = field(test, "at"),
                *start = field(test, "start");
    luaL_addstring(b, "v");
    if (day || days) {
        luaL_addstring(b, " and (");
        const cJSON *d = day ? day : days->child;
        for (;;) {
            luaL_addstring(b, "v.weekday == ");
            literal(b, d);
            if (day || !d->next)
                break;
            d = d->next;
            luaL_addstring(b, " or ");
        }
        luaL_addchar(b, ')');
    }
    if (at) {
        luaL_addstring(b, " and v.hour*60+v.minute == ");
        literal(b, at);
    } else if (start) {
        luaL_addstring(b, " and v.hour*60+v.minute >= ");
        literal(b, start);
        luaL_addstring(b, " and v.hour*60+v.minute < ");
        literal(b, field(test, "end"));
    }
}
void rule_source_push(lua_State *L, const cJSON *d)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    const cJSON *trigger = field(d, "when"), *action = field(d, "action");
    luaL_addstring(&b, "local previous,current=event.previous,event.current\n");
    if (cJSON_IsArray(trigger)) {
        luaL_addstring(&b, "local function ready(s)\n");
        const cJSON *test;
        cJSON_ArrayForEach(test, trigger)
        {
            luaL_addstring(&b, "  local v=s[");
            literal(&b, field(test, "name"));
            luaL_addstring(&b, "]\n");
            if (field(test, "clock")) {
                luaL_addstring(&b, "  if not (");
                clock_predicate(&b, test);
                luaL_addstring(&b, ") then return false end\n");
            } else {
                luaL_addstring(&b, "  if not (");
                comparison(&b, test, "v");
                luaL_addstring(&b, ") then return false end\n");
            }
        }
        luaL_addstring(&b, "  return true\nend\n");
        cJSON_ArrayForEach(test, trigger)
        {
            if (!field(test, "clock") || !field(test, "at"))
                continue;
            // At is a minute arrival, not a window that property changes can enter later in that minute.
            luaL_addstring(&b, "local p=previous[");
            literal(&b, field(test, "name"));
            luaL_addstring(&b, "]\nif p.hour*60+p.minute == ");
            literal(&b, field(test, "at"));
            luaL_addstring(&b, " then return end\n");
        }
        luaL_addstring(&b, "if ready(previous) or not ready(current) then return end\n");
    } else if (strcmp(cJSON_GetStringValue(field(trigger, "op")), "changes")) {
        luaL_addstring(&b, "  if (");
        comparison(&b, trigger, "previous");
        luaL_addstring(&b, ") or not (");
        comparison(&b, trigger, "current");
        luaL_addstring(&b, ") then return end\n");
    }
    const cJSON *condition;
    cJSON_ArrayForEach(condition, field(d, "conditions"))
    {
        if (field(condition, "clock")) {
            luaL_addstring(&b, "  local v=automation.local_time()\n  if not (");
            clock_predicate(&b, condition);
            luaL_addstring(&b, ") then return 'clock' end\n");
            continue;
        }
        luaL_addstring(&b, "  local condition = matter.get_property(");
        path(&b, condition, "property");
        luaL_addstring(&b, ")\n  if condition == nil");
        luaL_addstring(&b, " or not (");
        comparison(&b, condition, "condition");
        luaL_addchar(&b, ')');
        luaL_addstring(&b, " then return ");
        quoted(&b, cJSON_GetStringValue(field(condition, "property")));
        luaL_addstring(&b, " end\n");
    }
    if (cJSON_IsArray(action)) {
        const cJSON *step;
        cJSON_ArrayForEach(step, action) emit_step(&b, step);
    } else
        emit_step(&b, action);
    luaL_pushresult(&b);
}
