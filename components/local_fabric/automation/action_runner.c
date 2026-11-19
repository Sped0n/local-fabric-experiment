#include "action_runner.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    action_runner_t *owner;
    action_step_t *steps;
    int64_t due;
    bool pending, cancelled;
    esp_err_t result;
} action_slot_t;
struct action_runner {
    fabric_manager_handle_t manager;
    action_done_fn done;
    void *context;
    // Only completed + the callback results cross task boundaries. Never enter CHIP under this lock.
    SemaphoreHandle_t lock;
    TaskHandle_t worker;
    uint32_t completed;
    action_slot_t slots[FABRIC_AUTOMATION_SLOTS];
};
void action_steps_free(action_step_t *p)
{
    while (p) {
        action_step_t *next = p->next;
        cJSON_free(p->args);
        free(p);
        p = next;
    }
}
action_runner_t *action_runner_create(fabric_manager_handle_t m, action_done_fn done, void *ctx)
{
    action_runner_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r)
        return NULL;
    r->lock = xSemaphoreCreateMutex();
    if (!r->lock) {
        free(r);
        return NULL;
    }
    r->manager = m;
    r->done = done;
    r->context = ctx;
    for (unsigned i = 0; i < FABRIC_AUTOMATION_SLOTS; i++) r->slots[i].owner = r;
    return r;
}
void action_runner_cancel(action_runner_t *r, unsigned i)
{
    action_slot_t *s = &r->slots[i];
    action_steps_free(s->steps);
    s->steps = NULL;
    s->due = 0;
    s->cancelled = s->pending;
}
void action_runner_delete(action_runner_t *r)
{
    if (!r)
        return;
    for (unsigned i = 0; i < FABRIC_AUTOMATION_SLOTS; i++) {
        configASSERT(!r->slots[i].pending);
        action_runner_cancel(r, i);
    }
    vSemaphoreDelete(r->lock);
    free(r);
}
void action_runner_start(action_runner_t *r, unsigned i, action_step_t *steps)
{
    configASSERT(!action_runner_active(r, i));
    r->slots[i].steps = steps;
}
bool action_runner_active(action_runner_t *r, unsigned i)
{
    action_slot_t *s = &r->slots[i];
    return s->steps || s->pending || s->due;
}
static void completed(fabric_core_handle_t core, const fabric_core_request_result_t *result, void *ctx)
{
    action_slot_t *s = ctx;
    action_runner_t *r = s->owner;
    unsigned i = s - r->slots;
    xSemaphoreTake(r->lock, portMAX_DELAY);
    s->result = result->error;
    r->completed |= 1u << i;
    xTaskNotify(r->worker, 0, eNoAction);
    xSemaphoreGive(r->lock);
}
int64_t action_runner_advance(action_runner_t *r)
{
    xSemaphoreTake(r->lock, portMAX_DELAY);
    uint32_t completed_mask = r->completed;
    r->completed = 0;
    if (!r->worker)
        r->worker = xTaskGetCurrentTaskHandle();
    xSemaphoreGive(r->lock);
    int64_t earliest = INT64_MAX;
#if CONFIG_FABRIC_AUTOMATION_TIMING
    int64_t start = esp_timer_get_time(), max_late = 0;
    unsigned resumed = 0;
#endif
    for (unsigned i = 0; i < FABRIC_AUTOMATION_SLOTS; i++) {
        action_slot_t *s = &r->slots[i];
        esp_err_t err = ESP_OK;
        if (completed_mask & (1u << i)) {
            s->pending = false;
            if (s->cancelled) {
                s->cancelled = false;
                continue;
            }
            err = s->result;
        } else if (s->pending || (!s->steps && !s->due))
            continue;
        if (s->due) {
            int64_t now = esp_timer_get_time();
            if (now < s->due) {
                if (s->due < earliest)
                    earliest = s->due;
                continue;
            }
#if CONFIG_FABRIC_AUTOMATION_TIMING
            resumed++;
            if (now - s->due > max_late)
                max_late = now - s->due;
#endif
            s->due = 0;
        }
        while (err == ESP_OK && s->steps) {
            action_step_t *p = s->steps;
            s->steps = p->next;
            p->next = NULL;
            if (p->delay_seconds) {
                s->due = esp_timer_get_time() + INT64_C(1000000) * p->delay_seconds;
                if (s->due < earliest)
                    earliest = s->due;
            } else if (p->native)
                err = p->native->try_enqueue(p->native->context, p->args);
            else {
                err = fabric_manager_invoke_action(r->manager, p->device, p->endpoint, p->cluster, p->name, p->args,
                                                   completed, s);
                s->pending = err == ESP_OK;
            }
            action_steps_free(p);
            if (s->due || s->pending)
                break;
        }
        if (err != ESP_OK || (!s->pending && !s->due && !s->steps)) {
            action_runner_cancel(r, i);
            r->done(r->context, i, err);
        }
    }
#if CONFIG_FABRIC_AUTOMATION_TIMING
    if (resumed)
        ESP_LOGI("rule_timing", "resumed=%u max_late_us=%lld run_us=%lld", resumed, (long long)max_late,
                 (long long)(esp_timer_get_time() - start));
#endif
    return earliest;
}
