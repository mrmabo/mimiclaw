#pragma once

#include "bus/message_bus.h"
#include "esp_err.h"

esp_err_t app_voice_main_init(void);
esp_err_t app_voice_main_start(void);
void app_voice_main_handle_agent_response(const mimi_msg_t *msg);
