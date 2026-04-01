#pragma once

#include "bus/message_bus.h"
#include "esp_err.h"

esp_err_t voice_session_init(void);
esp_err_t voice_session_start(void);
void voice_session_handle_agent_response(const mimi_msg_t *msg);
