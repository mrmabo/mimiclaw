#include "voice/capture_controller.h"

#include "audio/audio_hal.h"
#include "esp_log.h"

static const char *TAG = "voice_capture";

static void on_mic_frame(const int16_t *pcm, size_t samples, void *ctx)
{
    capture_controller_t *controller = (capture_controller_t *)ctx;
    if (!controller || !controller->active || !controller->client) {
        return;
    }
    voice_ws_client_send_binary(controller->client,
                                (const uint8_t *)pcm,
                                samples * sizeof(int16_t));
}

esp_err_t capture_controller_init(capture_controller_t *controller)
{
    if (!controller) {
        return ESP_ERR_INVALID_ARG;
    }
    controller->consumer_handle = -1;
    controller->client = NULL;
    controller->active = false;
    return ESP_OK;
}

esp_err_t capture_controller_start(capture_controller_t *controller, voice_ws_client_t *client)
{
    if (!controller || !client) {
        return ESP_ERR_INVALID_ARG;
    }
    if (controller->active) {
        return ESP_OK;
    }

    controller->client = client;
    controller->consumer_handle = audio_hal_register_mic_consumer(on_mic_frame, controller);
    if (controller->consumer_handle < 0) {
        ESP_LOGE(TAG, "No free mic consumer slots");
        return ESP_FAIL;
    }

    controller->active = true;
    ESP_LOGI(TAG, "Voice capture started");
    return ESP_OK;
}

esp_err_t capture_controller_stop(capture_controller_t *controller)
{
    if (!controller) {
        return ESP_ERR_INVALID_ARG;
    }
    if (controller->consumer_handle >= 0) {
        audio_hal_unregister_mic_consumer(controller->consumer_handle);
        controller->consumer_handle = -1;
    }
    controller->client = NULL;
    controller->active = false;
    ESP_LOGI(TAG, "Voice capture stopped");
    return ESP_OK;
}

bool capture_controller_is_active(const capture_controller_t *controller)
{
    return controller && controller->active;
}
