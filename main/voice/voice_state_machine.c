#include "voice/voice_state_machine.h"

void voice_state_machine_init(voice_state_machine_t *sm)
{
    if (!sm) {
        return;
    }
    sm->current = VOICE_STATE_IDLE;
}

bool voice_state_machine_transition(voice_state_machine_t *sm, voice_state_t next)
{
    if (!sm) {
        return false;
    }
    sm->current = next;
    return true;
}

const char *voice_state_machine_name(voice_state_t state)
{
    switch (state) {
    case VOICE_STATE_IDLE:
        return "IDLE";
    case VOICE_STATE_WAKE_DETECTED:
        return "WAKE_DETECTED";
    case VOICE_STATE_ACK_PLAYING:
        return "ACK_PLAYING";
    case VOICE_STATE_LISTENING:
        return "LISTENING";
    case VOICE_STATE_UPLOADING:
        return "UPLOADING";
    case VOICE_STATE_THINKING:
        return "THINKING";
    case VOICE_STATE_SPEAKING:
        return "SPEAKING";
    case VOICE_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
