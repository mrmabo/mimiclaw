#pragma once

#include "audio/audio_hal.h"
#include "esp_err.h"
#include <stdbool.h>
#include "voice/voice_ws_client.h"

typedef struct {
    audio_hal_consumer_handle_t consumer_handle;
    voice_ws_client_t *client;
    bool active;
} capture_controller_t;

esp_err_t capture_controller_init(capture_controller_t *controller);
esp_err_t capture_controller_start(capture_controller_t *controller, voice_ws_client_t *client);
esp_err_t capture_controller_stop(capture_controller_t *controller);
bool capture_controller_is_active(const capture_controller_t *controller);
