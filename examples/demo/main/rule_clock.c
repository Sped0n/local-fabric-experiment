#include "rule_clock.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include <stdlib.h>
#include <time.h>

static void time_synced(void *service, esp_event_base_t base, int32_t id, void *event)
{
    fabric_automation_clock_synchronized(service);
    ESP_LOGI("demo", "Rule clock synchronized (TZ=%s)", CONFIG_DEMO_TIMEZONE);
}
esp_err_t demo_start_rule_clock(fabric_automation_t service)
{
    if (setenv("TZ", CONFIG_DEMO_TIMEZONE, 1) != 0)
        return ESP_ERR_NO_MEM;
    tzset();
    esp_err_t err = esp_event_handler_register(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, time_synced, service);
    if (err != ESP_OK)
        return err;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_DEMO_NTP_SERVER);
    config.wait_for_sync = false; // Non-clock rules never wait for the network clock.
    err = esp_netif_sntp_init(&config);
    if (err != ESP_OK)
        esp_event_handler_unregister(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, time_synced);
    return err;
}
