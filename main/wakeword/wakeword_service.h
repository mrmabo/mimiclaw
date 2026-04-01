#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "wakeword/wakeword_router.h"

typedef struct {
    int wake_word_index;
    int wakenet_model_index;
    int64_t detected_at_ms;
    const wakeword_route_t *route;
} wakeword_event_t;

typedef void (*wakeword_service_cb_t)(const wakeword_event_t *event, void *ctx);

esp_err_t wakeword_service_init(void);
esp_err_t wakeword_service_start(void);
esp_err_t wakeword_service_stop(void);
esp_err_t wakeword_service_enable(void);
esp_err_t wakeword_service_disable(void);
esp_err_t wakeword_service_register_callback(wakeword_service_cb_t cb, void *ctx);
bool wakeword_service_is_available(void);
