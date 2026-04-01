#pragma once

#include <stdbool.h>

typedef enum {
    VOICE_STATE_IDLE = 0,
    VOICE_STATE_WAKE_DETECTED,
    VOICE_STATE_ACK_PLAYING,
    VOICE_STATE_LISTENING,
    VOICE_STATE_UPLOADING,
    VOICE_STATE_THINKING,
    VOICE_STATE_SPEAKING,
    VOICE_STATE_ERROR,
} voice_state_t;

typedef struct {
    voice_state_t current;
} voice_state_machine_t;

void voice_state_machine_init(voice_state_machine_t *sm);
bool voice_state_machine_transition(voice_state_machine_t *sm, voice_state_t next);
const char *voice_state_machine_name(voice_state_t state);
