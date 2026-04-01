#include "wakeword/wakeword_service.h"

#include "audio/audio_hal.h"
#include "mimi_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if __has_include("esp_afe_sr_iface.h") && __has_include("esp_afe_sr_models.h") && __has_include("model_path.h")
#define MIMI_HAS_ESP_SR 1
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"
#include "model_path.h"
#else
#define MIMI_HAS_ESP_SR 0
#endif

static const char *TAG = "wakeword";

typedef struct {
    bool initialized;
    bool started;
    bool enabled;
    bool available;
    wakeword_service_cb_t cb;
    void *cb_ctx;

#if MIMI_HAS_ESP_SR
    srmodel_list_t *models;
    afe_config_t *afe_cfg;
    const esp_afe_sr_iface_t *afe_handle;
    esp_afe_sr_data_t *afe_data;
    audio_hal_consumer_handle_t mic_consumer;
    QueueHandle_t mic_queue;
    TaskHandle_t feed_task;
    TaskHandle_t fetch_task;
    int feed_chunk_samples;
    int16_t pending[MIMI_AUDIO_FRAME_SAMPLES];
    size_t pending_samples;
#endif
} wakeword_runtime_t;

static wakeword_runtime_t s_runtime;

#if MIMI_HAS_ESP_SR
typedef struct {
    size_t samples;
    int16_t pcm[MIMI_AUDIO_FRAME_SAMPLES];
} wakeword_pcm_block_t;

static void mic_frame_callback(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    if (!s_runtime.started || !s_runtime.enabled || !s_runtime.mic_queue) {
        return;
    }

    wakeword_pcm_block_t block = {0};
    block.samples = samples;
    if (block.samples > MIMI_AUDIO_FRAME_SAMPLES) {
        block.samples = MIMI_AUDIO_FRAME_SAMPLES;
    }
    memcpy(block.pcm, pcm, block.samples * sizeof(int16_t));
    xQueueSend(s_runtime.mic_queue, &block, 0);
}

static void resolve_model_names(afe_config_t *cfg, srmodel_list_t *models)
{
    /* TODO: The packaged ESP-SR bundle may not include the desired English wake word.
     * When that happens, keep the route entry but replace MIMI_WAKEWORD_SECONDARY_MODEL_HINT
     * with a custom WakeNet model name once it is integrated into the model partition.
     */
    cfg->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, MIMI_WAKEWORD_PRIMARY_MODEL_HINT);
    cfg->wakenet_model_name_2 = esp_srmodel_filter(models, ESP_WN_PREFIX, MIMI_WAKEWORD_SECONDARY_MODEL_HINT);

    if (!cfg->wakenet_model_name) {
        cfg->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    }
}

static esp_err_t init_afe(void)
{
    s_runtime.models = esp_srmodel_init(MIMI_WAKEWORD_MODEL_PARTITION_LABEL);
    if (!s_runtime.models) {
        ESP_LOGW(TAG, "No speech models found in partition '%s'", MIMI_WAKEWORD_MODEL_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    s_runtime.afe_cfg = afe_config_init("M", s_runtime.models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!s_runtime.afe_cfg) {
        return ESP_ERR_NO_MEM;
    }

    s_runtime.afe_cfg->aec_init = false;
    s_runtime.afe_cfg->se_init = false;
    s_runtime.afe_cfg->ns_init = false;
    s_runtime.afe_cfg->agc_init = false;
    s_runtime.afe_cfg->vad_init = true;
    s_runtime.afe_cfg->wakenet_init = true;
    s_runtime.afe_cfg->afe_ringbuf_size = 50;
    resolve_model_names(s_runtime.afe_cfg, s_runtime.models);
    afe_config_check(s_runtime.afe_cfg);

    if (!s_runtime.afe_cfg->wakenet_model_name) {
        ESP_LOGW(TAG, "No WakeNet model matched the configured hints");
        return ESP_ERR_NOT_FOUND;
    }

    s_runtime.afe_handle = esp_afe_handle_from_config(s_runtime.afe_cfg);
    if (!s_runtime.afe_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    s_runtime.afe_data = s_runtime.afe_handle->create_from_config(s_runtime.afe_cfg);
    if (!s_runtime.afe_data) {
        return ESP_ERR_NO_MEM;
    }

    s_runtime.feed_chunk_samples = s_runtime.afe_handle->get_feed_chunksize(s_runtime.afe_data);
    ESP_LOGI(TAG, "WakeNet ready with primary model '%s'%s",
             s_runtime.afe_cfg->wakenet_model_name,
             s_runtime.afe_cfg->wakenet_model_name_2 ? " and secondary model" : "");
    return ESP_OK;
}

static void feed_task(void *arg)
{
    (void)arg;
    wakeword_pcm_block_t block;

    while (1) {
        if (xQueueReceive(s_runtime.mic_queue, &block, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_runtime.enabled) {
            continue;
        }

        size_t offset = 0;
        while (offset < block.samples) {
            size_t needed = s_runtime.feed_chunk_samples - s_runtime.pending_samples;
            size_t to_copy = block.samples - offset;
            if (to_copy > needed) {
                to_copy = needed;
            }

            memcpy(&s_runtime.pending[s_runtime.pending_samples],
                   &block.pcm[offset],
                   to_copy * sizeof(int16_t));
            s_runtime.pending_samples += to_copy;
            offset += to_copy;

            if (s_runtime.pending_samples == (size_t)s_runtime.feed_chunk_samples) {
                s_runtime.afe_handle->feed(s_runtime.afe_data, s_runtime.pending);
                s_runtime.pending_samples = 0;
            }
        }
    }
}

static void fetch_task(void *arg)
{
    (void)arg;

    while (1) {
        afe_fetch_result_t *result = s_runtime.afe_handle->fetch_with_delay(
            s_runtime.afe_data,
            pdMS_TO_TICKS(200));
        if (!result || !s_runtime.enabled) {
            continue;
        }

        if (result->wakeup_state == WAKENET_DETECTED && s_runtime.cb) {
            const wakeword_route_t *route = wakeword_router_resolve(
                result->wakenet_model_index,
                result->wake_word_index);
            if (!route) {
                route = wakeword_router_default_route();
            }

            wakeword_event_t event = {
                .wake_word_index = result->wake_word_index,
                .wakenet_model_index = result->wakenet_model_index,
                .detected_at_ms = esp_timer_get_time() / 1000,
                .route = route,
            };

            ESP_LOGI(TAG, "Wake word detected: model=%d word=%d route=%s",
                     event.wakenet_model_index,
                     event.wake_word_index,
                     route ? route->label : "unknown");
            s_runtime.cb(&event, s_runtime.cb_ctx);
        }
    }
}

static void cleanup_afe(void)
{
    if (s_runtime.feed_task) {
        vTaskDelete(s_runtime.feed_task);
        s_runtime.feed_task = NULL;
    }
    if (s_runtime.fetch_task) {
        vTaskDelete(s_runtime.fetch_task);
        s_runtime.fetch_task = NULL;
    }
    if (s_runtime.mic_consumer >= 0) {
        audio_hal_unregister_mic_consumer(s_runtime.mic_consumer);
        s_runtime.mic_consumer = -1;
    }
    if (s_runtime.mic_queue) {
        vQueueDelete(s_runtime.mic_queue);
        s_runtime.mic_queue = NULL;
    }
    if (s_runtime.afe_handle && s_runtime.afe_data) {
        s_runtime.afe_handle->destroy(s_runtime.afe_data);
        s_runtime.afe_data = NULL;
    }
    if (s_runtime.afe_cfg) {
        afe_config_free(s_runtime.afe_cfg);
        s_runtime.afe_cfg = NULL;
    }
    if (s_runtime.models) {
        esp_srmodel_deinit(s_runtime.models);
        s_runtime.models = NULL;
    }
}
#endif

esp_err_t wakeword_service_init(void)
{
    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.initialized = true;
#if MIMI_HAS_ESP_SR
    s_runtime.mic_consumer = -1;
    esp_err_t err = init_afe();
    s_runtime.available = (err == ESP_OK);
    return err;
#else
    s_runtime.available = false;
    ESP_LOGW(TAG, "ESP-SR headers are unavailable in this build. WakeNet remains disabled.");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t wakeword_service_start(void)
{
    if (!s_runtime.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_runtime.available) {
        return ESP_ERR_NOT_SUPPORTED;
    }

#if MIMI_HAS_ESP_SR
    s_runtime.mic_queue = xQueueCreate(8, sizeof(wakeword_pcm_block_t));
    if (!s_runtime.mic_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_runtime.mic_consumer = audio_hal_register_mic_consumer(mic_frame_callback, NULL);
    if (s_runtime.mic_consumer < 0) {
        return ESP_FAIL;
    }

    if (xTaskCreate(feed_task, "ww_feed", 4096, NULL, 7, &s_runtime.feed_task) != pdPASS) {
        return ESP_FAIL;
    }
    if (xTaskCreate(fetch_task, "ww_fetch", 4096, NULL, 7, &s_runtime.fetch_task) != pdPASS) {
        return ESP_FAIL;
    }
#endif

    s_runtime.started = true;
    s_runtime.enabled = true;
    return ESP_OK;
}

esp_err_t wakeword_service_stop(void)
{
    s_runtime.started = false;
    s_runtime.enabled = false;
#if MIMI_HAS_ESP_SR
    cleanup_afe();
#endif
    return ESP_OK;
}

esp_err_t wakeword_service_enable(void)
{
    if (!s_runtime.started || !s_runtime.available) {
        return ESP_ERR_INVALID_STATE;
    }
    s_runtime.enabled = true;
#if MIMI_HAS_ESP_SR
    if (s_runtime.afe_handle && s_runtime.afe_data) {
        s_runtime.afe_handle->enable_wakenet(s_runtime.afe_data);
    }
#endif
    return ESP_OK;
}

esp_err_t wakeword_service_disable(void)
{
    s_runtime.enabled = false;
#if MIMI_HAS_ESP_SR
    if (s_runtime.afe_handle && s_runtime.afe_data) {
        s_runtime.afe_handle->disable_wakenet(s_runtime.afe_data);
    }
#endif
    return ESP_OK;
}

esp_err_t wakeword_service_register_callback(wakeword_service_cb_t cb, void *ctx)
{
    s_runtime.cb = cb;
    s_runtime.cb_ctx = ctx;
    return ESP_OK;
}

bool wakeword_service_is_available(void)
{
    return s_runtime.available;
}
