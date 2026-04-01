#include "voice/voice_session.h"

#include "bridge/agent_bridge.h"
#include "mimi_config.h"
#include "voice/capture_controller.h"
#include "voice/playback_controller.h"
#include "voice/voice_protocol.h"
#include "voice/voice_state_machine.h"
#include "voice/voice_ws_client.h"
#include "wakeword/wakeword_router.h"
#include "wakeword/wakeword_service.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "voice_session";

typedef struct {
    SemaphoreHandle_t lock;
    voice_state_machine_t sm;
    voice_ws_client_t ws;
    capture_controller_t capture;
    const wakeword_route_t *route;
    char session_id[64];
    bool started;
} voice_session_runtime_t;

static voice_session_runtime_t s_voice;

static void finish_session_locked(voice_state_t reason_state)
{
    capture_controller_stop(&s_voice.capture);
    playback_controller_flush();
    wakeword_service_enable();
    voice_state_machine_transition(&s_voice.sm, reason_state == VOICE_STATE_ERROR ? VOICE_STATE_ERROR : VOICE_STATE_IDLE);
    s_voice.route = NULL;
    s_voice.session_id[0] = '\0';
    if (reason_state == VOICE_STATE_ERROR) {
        voice_state_machine_transition(&s_voice.sm, VOICE_STATE_IDLE);
    }
}

static esp_err_t send_listen_start_locked(void)
{
    esp_err_t err = voice_ws_client_send_json(
        &s_voice.ws,
        voice_protocol_create_listen_start(
            s_voice.session_id,
            s_voice.route ? s_voice.route->preferred_session_language : MIMI_VOICE_DEFAULT_LANGUAGE));
    if (err != ESP_OK) {
        return err;
    }
    err = capture_controller_start(&s_voice.capture, &s_voice.ws);
    if (err == ESP_OK) {
        voice_state_machine_transition(&s_voice.sm, VOICE_STATE_LISTENING);
    }
    return err;
}

static void begin_session_from_wake_locked(const wakeword_event_t *event)
{
    if (!event || !event->route) {
        return;
    }
    if (s_voice.sm.current != VOICE_STATE_IDLE) {
        ESP_LOGW(TAG, "Ignoring wake word while session is active");
        return;
    }

    s_voice.route = event->route;
    snprintf(s_voice.session_id, sizeof(s_voice.session_id), "%s-%08x",
             MIMI_VOICE_SESSION_ID_PREFIX, (unsigned)esp_random());

    wakeword_service_disable();
    voice_state_machine_transition(&s_voice.sm, VOICE_STATE_WAKE_DETECTED);
    voice_state_machine_transition(&s_voice.sm, VOICE_STATE_ACK_PLAYING);
    playback_controller_play_local_clip(event->route->ack_clip);

    if (!voice_ws_client_is_connected(&s_voice.ws)) {
        ESP_LOGW(TAG, "Gateway is not connected, aborting voice session");
        finish_session_locked(VOICE_STATE_ERROR);
        return;
    }

    if (MIMI_VOICE_START_UPLOAD_AFTER_ACK) {
        if (send_listen_start_locked() != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start capture after ack");
            finish_session_locked(VOICE_STATE_ERROR);
        }
    }
}

static void on_wakeword_detected(const wakeword_event_t *event, void *ctx)
{
    (void)ctx;
    if (!s_voice.lock) {
        return;
    }
    if (xSemaphoreTake(s_voice.lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        begin_session_from_wake_locked(event);
        xSemaphoreGive(s_voice.lock);
    }
}

static void on_gateway_connected(void *ctx)
{
    (void)ctx;
    voice_ws_client_send_json(&s_voice.ws, voice_protocol_create_hello());
}

static void on_gateway_disconnected(void *ctx, esp_err_t err)
{
    (void)ctx;
    ESP_LOGW(TAG, "Gateway disconnected: %s", esp_err_to_name(err));
    if (xSemaphoreTake(s_voice.lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_voice.sm.current != VOICE_STATE_IDLE) {
            finish_session_locked(VOICE_STATE_ERROR);
        }
        xSemaphoreGive(s_voice.lock);
    }
}

static void on_gateway_audio(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (xSemaphoreTake(s_voice.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_voice.sm.current == VOICE_STATE_THINKING || s_voice.sm.current == VOICE_STATE_SPEAKING) {
            voice_state_machine_transition(&s_voice.sm, VOICE_STATE_SPEAKING);
            playback_controller_enqueue_pcm(data, len);
        }
        xSemaphoreGive(s_voice.lock);
    }
}

static void on_gateway_json(void *ctx, cJSON *root)
{
    (void)ctx;
    const char *type = voice_protocol_get_type(root);
    if (!type) {
        return;
    }

    if (xSemaphoreTake(s_voice.lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    if (strcmp(type, "stt_partial") == 0) {
        /* Reserved for UI/logging later. */
    } else if (strcmp(type, "stt_final") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring[0]) {
            capture_controller_stop(&s_voice.capture);
            voice_state_machine_transition(&s_voice.sm, VOICE_STATE_THINKING);
            agent_bridge_submit_voice_text(
                s_voice.session_id,
                text->valuestring,
                s_voice.route ? s_voice.route->preferred_session_language : MIMI_VOICE_DEFAULT_LANGUAGE);
        }
    } else if (strcmp(type, "tts_start") == 0 || strcmp(type, "tts_sentence_start") == 0) {
        voice_state_machine_transition(&s_voice.sm, VOICE_STATE_SPEAKING);
    } else if (strcmp(type, "tts_stop") == 0 || strcmp(type, "abort") == 0) {
        finish_session_locked(VOICE_STATE_IDLE);
    } else if (strcmp(type, "listen_stop") == 0) {
        capture_controller_stop(&s_voice.capture);
        voice_state_machine_transition(&s_voice.sm, VOICE_STATE_UPLOADING);
    } else if (strcmp(type, "llm_thinking") == 0) {
        voice_state_machine_transition(&s_voice.sm, VOICE_STATE_THINKING);
    } else if (strcmp(type, "error") == 0) {
        finish_session_locked(VOICE_STATE_ERROR);
    }

    xSemaphoreGive(s_voice.lock);
}

esp_err_t voice_session_init(void)
{
    memset(&s_voice, 0, sizeof(s_voice));
    s_voice.lock = xSemaphoreCreateMutex();
    if (!s_voice.lock) {
        return ESP_ERR_NO_MEM;
    }

    voice_state_machine_init(&s_voice.sm);
    capture_controller_init(&s_voice.capture);

    voice_ws_client_callbacks_t callbacks = {
        .on_connected = on_gateway_connected,
        .on_disconnected = on_gateway_disconnected,
        .on_json = on_gateway_json,
        .on_audio = on_gateway_audio,
    };

    esp_err_t err = voice_ws_client_init(&s_voice.ws, MIMI_VOICE_GATEWAY_URL, &callbacks, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice gateway client disabled: %s", esp_err_to_name(err));
        return err;
    }

    err = playback_controller_init();
    if (err != ESP_OK) {
        return err;
    }

    return wakeword_service_register_callback(on_wakeword_detected, NULL);
}

esp_err_t voice_session_start(void)
{
    if (s_voice.started) {
        return ESP_OK;
    }
    s_voice.started = true;
    return voice_ws_client_start(&s_voice.ws);
}

void voice_session_handle_agent_response(const mimi_msg_t *msg)
{
    if (!msg || !msg->content) {
        return;
    }
    if (xSemaphoreTake(s_voice.lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    if (s_voice.session_id[0] == '\0' || strcmp(s_voice.session_id, msg->chat_id) != 0) {
        xSemaphoreGive(s_voice.lock);
        return;
    }

    voice_state_machine_transition(&s_voice.sm, VOICE_STATE_THINKING);
    voice_ws_client_send_json(
        &s_voice.ws,
        voice_protocol_create_tts_request(
            s_voice.session_id,
            msg->content,
            msg->preferred_language[0] ? msg->preferred_language :
                (s_voice.route ? s_voice.route->preferred_session_language : MIMI_VOICE_DEFAULT_LANGUAGE)));

    xSemaphoreGive(s_voice.lock);
}
