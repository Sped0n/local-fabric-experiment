#include "json_memory.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

static void *json_alloc(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void demo_init_json_memory(void)
{
    // Process-global hooks: install once, before Wi-Fi, CHIP, or any JSON user starts.
    cJSON_Hooks hooks = {.malloc_fn = json_alloc, .free_fn = heap_caps_free};
    cJSON_InitHooks(&hooks);
}
