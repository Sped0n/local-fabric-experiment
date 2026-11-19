#include "console.h"
#include "demo_automation.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "fabric_http.h"
#include "json_memory.h"
#include "nvs_flash.h"
#include "rule_clock.h"
#include <string.h>

// rc0.2 reads these IDF buffers as C strings. Require space for both terminators.
_Static_assert(sizeof(CONFIG_DEMO_SSID) <= sizeof(((wifi_sta_config_t *)0)->ssid), "SSID too long");
_Static_assert(sizeof(CONFIG_DEMO_PASSWORD) <= sizeof(((wifi_sta_config_t *)0)->password), "password too long");

static void got_ip(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = data;
    ESP_LOGI("demo", "http://" IPSTR, IP2STR(&event->ip_info.ip));
}

void app_main(void)
{
    demo_init_json_memory();
    ESP_ERROR_CHECK(nvs_flash_init()); // Never erase an existing fabric on startup failure.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip, NULL));
    if (!esp_netif_create_default_wifi_sta())
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t network = {.sta = {.ssid = CONFIG_DEMO_SSID, .password = CONFIG_DEMO_PASSWORD}};
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &network));
    ESP_ERROR_CHECK(esp_wifi_start());
    // CHIP drives STA reconnection after stack initialization.
    fabric_core_config_t config = {.max_nodes = 8, .subscription_interval_s_min = 0, .subscription_interval_s_max = 30};
    fabric_core_handle_t core;
    fabric_manager_handle_t manager;
    ESP_ERROR_CHECK(fabric_core_create(&config, &core));
    ESP_ERROR_CHECK(fabric_manager_create(core, &manager));
    ESP_ERROR_CHECK(fabric_core_start(core));
    fabric_automation_t automation = NULL;
#if CONFIG_FABRIC_AUTOMATION
    ESP_ERROR_CHECK(demo_start_automation(manager, &automation));
    esp_err_t clock_err = demo_start_rule_clock(automation);
    if (clock_err != ESP_OK)
        ESP_LOGW("demo", "Rule clock unavailable: %s", esp_err_to_name(clock_err));
#endif
    ESP_ERROR_CHECK(fabric_http_start(core, manager, automation));
    ESP_ERROR_CHECK(demo_console_start());
}
