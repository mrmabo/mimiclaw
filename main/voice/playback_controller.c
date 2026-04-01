#include "voice/playback_controller.h"

#include "audio/audio_hal.h"
#include "assets/ack_im_here.h"
#include "assets/ack_wozai.h"
#include "mimi_config.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "voice_playback";

typedef struct {
    size_t len;
    uint8_t data[MIMI_AUDIO_PLAYBACK_CHUNK_BYTES];
} playback_chunk_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;

static void playback_task(void *arg)
{
    (void)arg;
    playback_chunk_t chunk;
    while (1) {
        if (xQueueReceive(s_queue, &chunk, portMAX_DELAY) == pdTRUE) {
            audio_hal_play_bytes_blocking(chunk.data, chunk.len);
        }
    }
}

esp_err_t playback_controller_init(void)
{
    if (s_queue) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(MIMI_AUDIO_PLAYBACK_QUEUE_LEN, sizeof(playback_chunk_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(playback_task, "voice_play", 4096, NULL, 6, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t playback_controller_enqueue_pcm(const uint8_t *data, size_t len)
{
    if (!s_queue || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    while (offset < len) {
        playback_chunk_t chunk = {0};
        chunk.len = len - offset;
        if (chunk.len > sizeof(chunk.data)) {
            chunk.len = sizeof(chunk.data);
        }
        memcpy(chunk.data, data + offset, chunk.len);
        if (xQueueSend(s_queue, &chunk, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "Playback queue full");
            return ESP_ERR_TIMEOUT;
        }
        offset += chunk.len;
    }
    return ESP_OK;
}

esp_err_t playback_controller_play_local_clip(audio_ack_clip_id_t clip_id)
{
    switch (clip_id) {
    case AUDIO_ACK_CLIP_ZH_WO_ZAI:
        return playback_controller_enqueue_pcm((const uint8_t *)g_ack_wozai_pcm,
                                               g_ack_wozai_pcm_samples * sizeof(int16_t));
    case AUDIO_ACK_CLIP_EN_IM_HERE:
        return playback_controller_enqueue_pcm((const uint8_t *)g_ack_im_here_pcm,
                                               g_ack_im_here_pcm_samples * sizeof(int16_t));
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

void playback_controller_flush(void)
{
    if (s_queue) {
        xQueueReset(s_queue);
    }
}
