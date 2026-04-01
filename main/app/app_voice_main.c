#include "app/app_voice_main.h"

#include "audio/audio_hal.h"
#include "esp_log.h"
#include "voice/voice_session.h"
#include "wakeword/wakeword_service.h"

static const char *TAG = "app_voice";

esp_err_t app_voice_main_init(void)
{
    esp_err_t err = audio_hal_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice runtime disabled at audio init: %s", esp_err_to_name(err));
        return err;
    }

    err = audio_hal_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice runtime disabled at audio start: %s", esp_err_to_name(err));
        return err;
    }

    err = wakeword_service_init();
    if (err != ESP_OK) {
        return err;
    }

    err = voice_session_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice session init failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t app_voice_main_start(void)
{
    esp_err_t err = voice_session_start();
    if (err != ESP_OK) {
        return err;
    }

    err = wakeword_service_start();
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Wake word service unavailable in this build. Install/configure ESP-SR models to enable local wake.");
        return ESP_OK;
    }

    return err;
}

void app_voice_main_handle_agent_response(const mimi_msg_t *msg)
{
    voice_session_handle_agent_response(msg);
}
