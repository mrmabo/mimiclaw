#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*audio_hal_mic_frame_cb_t)(const int16_t *pcm, size_t samples, void *ctx);
typedef int audio_hal_consumer_handle_t;

esp_err_t audio_hal_init(void);
esp_err_t audio_hal_start(void);
void audio_hal_stop(void);
bool audio_hal_is_ready(void);

audio_hal_consumer_handle_t audio_hal_register_mic_consumer(audio_hal_mic_frame_cb_t cb, void *ctx);
void audio_hal_unregister_mic_consumer(audio_hal_consumer_handle_t handle);

esp_err_t audio_hal_play_pcm_blocking(const int16_t *pcm, size_t samples);
esp_err_t audio_hal_play_bytes_blocking(const void *data, size_t bytes);
