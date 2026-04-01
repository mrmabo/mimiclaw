#pragma once

#include "esp_err.h"

esp_err_t agent_bridge_submit_voice_text(const char *session_id,
                                         const char *text,
                                         const char *preferred_language);
