#include "runner.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    automation_runner_t *owner;
    automation_step_t *steps;
    int64_t due;
    bool pending, cancelled;
    uint8_t position;
    esp_err_t result;
} automation_runner_slot_t;
struct automation_runner {
    fabric_manager_handle_t manager;
    automation_runner_done_fn done;
    void *context;
    // Only completed + the callback results cross task boundaries. Never enter CHIP under this lock.
    SemaphoreHandle_t lock;
    TaskHandle_t worker;
    uint32_t completed;
    automation_runner_slot_t slots[FABRIC_AUTOMATION_SLOTS];
};
void automation_steps_free(automation_step_t *p)
{
    while (p) {
        automation_step_t *next = p->next;
        cJSON_free(p->args);
        free(p);
        p = next;
    }
}
automation_runner_t *automation_runner_create(fabric_manager_handle_t m, automation_runner_done_fn done, void *ctx)
{
    automation_runner_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
void automation_runner_cancel(automation_runner_t *r, unsigned i)
{
    automation_runner_slot_t *s = &r->slots[i];
    automation_steps_free(s->steps);
    s->steps = NULL;
    s->due = 0;
    s->cancelled = s->pending;
}
void automation_runner_delete(automation_runner_t *r)
{
    if (!r)
        return;
    for (unsigned i = 0; i < FABRIC_AUTOMATION_SLOTS; i++) {
        configASSERT(!r->slots[i].pending);
        automation_runner_cancel(r, i);
    }
    vSemaphoreDelete(r->lock);
    free(r);
}
void automation_runner_start(automation_runner_t *r, unsigned i, automation_step_t *steps)
{
    configASSERT(!automation_runner_active(r, i));
    r->slots[i].steps = steps;
    r->slots[i].position = 1;
}
bool automation_runner_active(automation_runner_t *r, unsigned i)
{
    automation_runner_slot_t *s = &r->slots[i];
    return s->steps || s->pending || s->due;
}
static void completed(fabric_core_handle_t core, const fabric_core_request_result_t *result, void *ctx)
{
    automation_runner_slot_t *s = ctx;
    automation_runner_t *r = s->owner;
    unsigned i = s - r->slots;
    xSemaphoreTake(r->lock, portMAX_DELAY);
    s->result = result->error;
    r->completed |= 1u << i;
    xTaskNotify(r->worker, 0, eNoAction);
    xSemaphoreGive(r->lock);
}
static void advance_step(automation_runner_slot_t *s)
{
    automation_step_t *done = s->steps;
    s->steps = done->next;
    done->next = NULL;
    automation_steps_free(done);
    s->position++;
}
int64_t automation_runner_advance(automation_runner_t *r)
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
        automation_runner_slot_t *s = &r->slots[i];
        esp_err_t err = ESP_OK;
        if (completed_mask & (1u << i)) {
            s->pending = false;
            if (s->cancelled) {
                s->cancelled = false;
                continue;
            }
            err = s->result;
            if (err == ESP_OK)
                advance_step(s);
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
            advance_step(s);
        }
        while (err == ESP_OK && s->steps) {
            automation_step_t *p = s->steps;
            if (p->delay_seconds) {
                s->due = esp_timer_get_time() + INT64_C(1000000) * p->delay_seconds;
                if (s->due < earliest)
                    earliest = s->due;
            } else if (p->hook) {
                err = p->hook->try_enqueue(p->hook->context, p->args);
            } else {
                err = fabric_manager_invoke_action(r->manager, p->device, p->endpoint, p->cluster, p->name, p->args,
                                                   completed, s);
                s->pending = err == ESP_OK;
            }
            if (s->due || s->pending || err != ESP_OK)
                break;
            advance_step(s);
        }
        if (err != ESP_OK || (!s->pending && !s->due && !s->steps)) {
            r->done(r->context, i, err, s->position, s->steps);
            automation_runner_cancel(r, i);
        }
    }
#if CONFIG_FABRIC_AUTOMATION_TIMING
    if (resumed)
        ESP_LOGI("rule_timing", "resumed=%u max_late_us=%lld run_us=%lld", resumed, (long long)max_late,
                 (long long)(esp_timer_get_time() - start));
#endif
    return earliest;
}
