#pragma once
#include "fabric_automation.h"

// Register the demo's local action consumer and start automation with its catalog.
esp_err_t demo_start_automation(fabric_manager_handle_t manager, fabric_automation_t *out);
