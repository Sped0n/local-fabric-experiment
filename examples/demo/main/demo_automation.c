#include "demo_automation.h"
#include "esp_event.h"
#include "esp_log.h"
#include <string.h>

ESP_EVENT_DEFINE_BASE(DEMO_NOTE);
static void log_note(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    ESP_LOGI("demo.note", "%s", (const char *)data);
}
static esp_err_t enqueue_note(void *ctx, const char *args)
{
    // The event loop copies the payload. A full queue fails the rule step immediately.
    return esp_event_post(DEMO_NOTE, 0, args, strlen(args) + 1, 0);
}
esp_err_t demo_start_automation(fabric_manager_handle_t manager, fabric_automation_t *out)
{
    static const fabric_automation_arg_t fields[] = {{"text", FABRIC_AUTOMATION_ARG_STRING}};
    static const fabric_automation_hook_t actions[] = {{.name = "demo.enqueue_note",
                                                        .description = "Queue a text note for the board console.",
                                                        .arguments = fields,
                                                        .argument_count = 1,
                                                        .try_enqueue = enqueue_note}};
    esp_err_t err = esp_event_handler_register(DEMO_NOTE, 0, log_note, NULL);
    if (err != ESP_OK)
        return err;
    err = fabric_automation_init(manager, actions, 1, out);
    if (err != ESP_OK)
        esp_event_handler_unregister(DEMO_NOTE, 0, log_note);
    return err;
}
