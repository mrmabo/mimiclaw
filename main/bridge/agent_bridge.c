#include "bridge/agent_bridge.h"

#include <stdlib.h>
#include <string.h>
#include "bus/message_bus.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "agent_bridge";

esp_err_t agent_bridge_submit_voice_text(const char *session_id,
                                         const char *text,
                                         const char *preferred_language)
{
    if (!session_id || !session_id[0] || !text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    mimi_msg_t msg;
    message_bus_init_msg(&msg);
    strncpy(msg.channel, MIMI_CHAN_VOICE, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, session_id, sizeof(msg.chat_id) - 1);
    strncpy(msg.session_id, session_id, sizeof(msg.session_id) - 1);
    strncpy(msg.source_role, "user", sizeof(msg.source_role) - 1);
    if (preferred_language && preferred_language[0]) {
        strncpy(msg.preferred_language, preferred_language, sizeof(msg.preferred_language) - 1);
    }
    msg.msg_type = MIMI_MSG_TYPE_VOICE_TEXT_FINAL;
    msg.flags = MIMI_MSG_FLAG_FINAL | MIMI_MSG_FLAG_FROM_USER;
    msg.timestamp_ms = esp_timer_get_time() / 1000;
    msg.content = strdup(text);
    if (!msg.content) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = message_bus_push_inbound(&msg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue voice text for agent: %s", esp_err_to_name(err));
        free(msg.content);
    }
    return err;
}
