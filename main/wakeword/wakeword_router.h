#pragma once

#include <stdbool.h>

typedef enum {
    AUDIO_ACK_CLIP_NONE = 0,
    AUDIO_ACK_CLIP_ZH_WO_ZAI,
    AUDIO_ACK_CLIP_EN_IM_HERE,
} audio_ack_clip_id_t;

typedef struct {
    int wake_word_index;
    int wakenet_model_index;
    const char *label;
    const char *language;
    const char *preferred_session_language;
    audio_ack_clip_id_t ack_clip;
    bool available;
    bool requires_custom_model;
} wakeword_route_t;

const wakeword_route_t *wakeword_router_resolve(int wakenet_model_index, int wake_word_index);
const wakeword_route_t *wakeword_router_default_route(void);
int wakeword_router_route_count(void);
const wakeword_route_t *wakeword_router_route_at(int index);
