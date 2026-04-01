#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_websocket_client.h"

typedef struct {
    void (*on_connected)(void *ctx);
    void (*on_disconnected)(void *ctx, esp_err_t err);
    void (*on_json)(void *ctx, cJSON *root);
    void (*on_audio)(void *ctx, const uint8_t *data, size_t len);
} voice_ws_client_callbacks_t;

typedef struct voice_ws_client {
    esp_websocket_client_handle_t handle;
    voice_ws_client_callbacks_t callbacks;
    void *ctx;
    char url[192];
    bool connected;
    char *text_buf;
    size_t text_cap;
} voice_ws_client_t;

esp_err_t voice_ws_client_init(voice_ws_client_t *client,
                               const char *url,
                               const voice_ws_client_callbacks_t *callbacks,
                               void *ctx);
esp_err_t voice_ws_client_start(voice_ws_client_t *client);
esp_err_t voice_ws_client_stop(voice_ws_client_t *client);
bool voice_ws_client_is_connected(const voice_ws_client_t *client);
esp_err_t voice_ws_client_send_json(voice_ws_client_t *client, cJSON *root);
esp_err_t voice_ws_client_send_binary(voice_ws_client_t *client, const uint8_t *data, size_t len);
