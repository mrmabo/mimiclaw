#include "audio/audio_hal.h"
#include "mimi_config.h"

#include <string.h>
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio_hal";

typedef struct {
    bool active;
    audio_hal_mic_frame_cb_t cb;
    void *ctx;
} mic_consumer_t;

static i2s_chan_handle_t s_rx_chan;
static i2s_chan_handle_t s_tx_chan;
static TaskHandle_t s_mic_task;
static SemaphoreHandle_t s_lock;
static mic_consumer_t s_consumers[MIMI_AUDIO_MIC_MAX_CONSUMERS];
static bool s_ready;

static bool audio_pins_configured(void)
{
    return MIMI_AUDIO_MIC_BCLK_PIN >= 0 &&
        MIMI_AUDIO_MIC_WS_PIN >= 0 &&
        MIMI_AUDIO_MIC_DIN_PIN >= 0 &&
        MIMI_AUDIO_SPK_BCLK_PIN >= 0 &&
        MIMI_AUDIO_SPK_WS_PIN >= 0 &&
        MIMI_AUDIO_SPK_DOUT_PIN >= 0;
}

static esp_err_t init_rx_channel(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIMI_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = MIMI_AUDIO_MIC_MCLK_PIN,
            .bclk = MIMI_AUDIO_MIC_BCLK_PIN,
            .ws = MIMI_AUDIO_MIC_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = MIMI_AUDIO_MIC_DIN_PIN,
        },
    };

    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }

    return i2s_channel_enable(s_rx_chan);
}

static esp_err_t init_tx_channel(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIMI_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = MIMI_AUDIO_SPK_MCLK_PIN,
            .bclk = MIMI_AUDIO_SPK_BCLK_PIN,
            .ws = MIMI_AUDIO_SPK_WS_PIN,
            .dout = MIMI_AUDIO_SPK_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }

    return i2s_channel_enable(s_tx_chan);
}

static void mic_capture_task(void *arg)
{
    (void)arg;

    int16_t frame[MIMI_AUDIO_FRAME_SAMPLES];
    ESP_LOGI(TAG, "Microphone capture task started");

    while (1) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            s_rx_chan,
            frame,
            sizeof(frame),
            &bytes_read,
            MIMI_AUDIO_IO_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Mic read failed: %s", esp_err_to_name(err));
            continue;
        }
        if (bytes_read == 0) {
            continue;
        }

        size_t samples = bytes_read / sizeof(int16_t);
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            for (int i = 0; i < MIMI_AUDIO_MIC_MAX_CONSUMERS; i++) {
                if (s_consumers[i].active && s_consumers[i].cb) {
                    s_consumers[i].cb(frame, samples, s_consumers[i].ctx);
                }
            }
            xSemaphoreGive(s_lock);
        }
    }
}

esp_err_t audio_hal_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!audio_pins_configured()) {
        ESP_LOGW(TAG, "Audio pins are not configured. Set the MIMI_AUDIO_*_PIN macros for your ESP32-S3 board.");
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = init_rx_channel();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S microphone: %s", esp_err_to_name(err));
        return err;
    }

    err = init_tx_channel();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S speaker: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "Audio HAL ready at %d Hz mono PCM", MIMI_AUDIO_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t audio_hal_start(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mic_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(
        mic_capture_task,
        "audio_mic",
        MIMI_AUDIO_MIC_TASK_STACK,
        NULL,
        MIMI_AUDIO_MIC_TASK_PRIO,
        &s_mic_task);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void audio_hal_stop(void)
{
    if (s_mic_task) {
        vTaskDelete(s_mic_task);
        s_mic_task = NULL;
    }
}

bool audio_hal_is_ready(void)
{
    return s_ready;
}

audio_hal_consumer_handle_t audio_hal_register_mic_consumer(audio_hal_mic_frame_cb_t cb, void *ctx)
{
    if (!cb || !s_lock) {
        return -1;
    }

    audio_hal_consumer_handle_t handle = -1;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < MIMI_AUDIO_MIC_MAX_CONSUMERS; i++) {
            if (!s_consumers[i].active) {
                s_consumers[i].active = true;
                s_consumers[i].cb = cb;
                s_consumers[i].ctx = ctx;
                handle = i;
                break;
            }
        }
        xSemaphoreGive(s_lock);
    }

    return handle;
}

void audio_hal_unregister_mic_consumer(audio_hal_consumer_handle_t handle)
{
    if (!s_lock || handle < 0 || handle >= MIMI_AUDIO_MIC_MAX_CONSUMERS) {
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        memset(&s_consumers[handle], 0, sizeof(s_consumers[handle]));
        xSemaphoreGive(s_lock);
    }
}

esp_err_t audio_hal_play_bytes_blocking(const void *data, size_t bytes)
{
    if (!s_ready || !data || bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *ptr = (const uint8_t *)data;
    size_t remaining = bytes;

    while (remaining > 0) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, ptr, remaining, &written, MIMI_AUDIO_IO_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Speaker write failed: %s", esp_err_to_name(err));
            return err;
        }
        ptr += written;
        remaining -= written;
    }

    return ESP_OK;
}

esp_err_t audio_hal_play_pcm_blocking(const int16_t *pcm, size_t samples)
{
    return audio_hal_play_bytes_blocking(pcm, samples * sizeof(int16_t));
}
