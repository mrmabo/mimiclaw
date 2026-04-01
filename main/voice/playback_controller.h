#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include "wakeword/wakeword_router.h"

esp_err_t playback_controller_init(void);
esp_err_t playback_controller_play_local_clip(audio_ack_clip_id_t clip_id);
esp_err_t playback_controller_enqueue_pcm(const uint8_t *data, size_t len);
void playback_controller_flush(void);
