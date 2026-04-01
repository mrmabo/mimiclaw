#include "voice/voice_ws_client.h"
#include "mimi_config.h"

#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "voice_ws";

static void reset_text_buffer(voice_ws_client_t *client, size_t wanted)
{
    if (wanted == 0) {
        free(client->text_buf);
        client->text_buf = NULL;
        client->text_cap = 0;
        return;
    }

    if (client->text_cap >= wanted + 1) {
        memset(client->text_buf, 0, client->text_cap);
        return;
    }

    char *buf = realloc(client->text_buf, wanted + 1);
    if (!buf) {
        return;
    }
    memset(buf, 0, wanted + 1);
    client->text_buf = buf;
    client->text_cap = wanted + 1;
}

static void voice_ws_event_handler(void *handler_args,
                                   esp_event_base_t base,
                                   int32_t event_id,
                                   void *event_data)
{
    (void)base;
    voice_ws_client_t *client = (voice_ws_client_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        client->connected = true;
        ESP_LOGI(TAG, "Gateway websocket connected");
        if (client->callbacks.on_connected) {
            client->callbacks.on_connected(client->ctx);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        client->connected = false;
        ESP_LOGW(TAG, "Gateway websocket disconnected");
        if (client->callbacks.on_disconnected) {
            client->callbacks.on_disconnected(client->ctx, ESP_FAIL);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (!data || !data->data_ptr || data->data_len <= 0) {
            break;
        }

        if (data->op_code == 0x1) {
            if (data->payload_offset == 0) {
                reset_text_buffer(client, data->payload_len);
            }
            if (client->text_buf &&
                (size_t)(data->payload_offset + data->data_len) < client->text_cap) {
                memcpy(client->text_buf + data->payload_offset, data->data_ptr, data->data_len);
            }
            if (data->fin && client->text_buf) {
                cJSON *root = cJSON_Parse(client->text_buf);
                if (root && client->callbacks.on_json) {
                    client->callbacks.on_json(client->ctx, root);
                }
                cJSON_Delete(root);
                reset_text_buffer(client, 0);
            }
        } else if (data->op_code == 0x2) {
            if (client->callbacks.on_audio) {
                client->callbacks.on_audio(client->ctx,
                                           (const uint8_t *)data->data_ptr,
                                           (size_t)data->data_len);
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        client->connected = false;
        ESP_LOGW(TAG, "Gateway websocket error");
        break;
    default:
        break;
    }
}

esp_err_t voice_ws_client_init(voice_ws_client_t *client,
                               const char *url,
                               const voice_ws_client_callbacks_t *callbacks,
                               void *ctx)
{
    if (!client || !url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(client, 0, sizeof(*client));
    if (callbacks) {
        client->callbacks = *callbacks;
    }
    client->ctx = ctx;
    strncpy(client->url, url, sizeof(client->url) - 1);

    esp_websocket_client_config_t cfg = {
        .uri = client->url,
        .task_stack = MIMI_VOICE_GATEWAY_TASK_STACK,
        .buffer_size = MIMI_VOICE_GATEWAY_BUFFER_SIZE,
        .network_timeout_ms = MIMI_VOICE_GATEWAY_TIMEOUT_MS,
        .reconnect_timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client->handle = esp_websocket_client_init(&cfg);
    if (!client->handle) {
        return ESP_FAIL;
    }

    return esp_websocket_register_events(client->handle,
                                         WEBSOCKET_EVENT_ANY,
                                         voice_ws_event_handler,
                                         client);
}

esp_err_t voice_ws_client_start(voice_ws_client_t *client)
{
    if (!client || !client->handle) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_websocket_client_start(client->handle);
}

esp_err_t voice_ws_client_stop(voice_ws_client_t *client)
{
    if (!client || !client->handle) {
        return ESP_OK;
    }
    client->connected = false;
    esp_websocket_client_stop(client->handle);
    esp_websocket_client_destroy(client->handle);
    client->handle = NULL;
    reset_text_buffer(client, 0);
    return ESP_OK;
}

bool voice_ws_client_is_connected(const voice_ws_client_t *client)
{
    return client && client->connected;
}

esp_err_t voice_ws_client_send_json(voice_ws_client_t *client, cJSON *root)
{
    if (!client || !client->handle || !root) {
        return ESP_ERR_INVALID_ARG;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    int sent = esp_websocket_client_send_text(client->handle,
                                              json,
                                              (int)strlen(json),
                                              pdMS_TO_TICKS(1000));
    free(json);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t voice_ws_client_send_binary(voice_ws_client_t *client, const uint8_t *data, size_t len)
{
    if (!client || !client->handle || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int sent = esp_websocket_client_send_bin(client->handle,
                                             (const char *)data,
                                             (int)len,
                                             pdMS_TO_TICKS(1000));
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}
