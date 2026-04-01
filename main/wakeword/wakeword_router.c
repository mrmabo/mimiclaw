#include "wakeword/wakeword_router.h"
#include "mimi_config.h"
#include <stddef.h>

static const wakeword_route_t s_routes[] = {
    {
        .wake_word_index = MIMI_WAKEWORD_ZH_WORD_INDEX,
        .wakenet_model_index = MIMI_WAKEWORD_ZH_MODEL_INDEX,
        .label = MIMI_WAKEWORD_ZH_LABEL,
        .language = MIMI_WAKEWORD_ZH_LANGUAGE,
        .preferred_session_language = MIMI_WAKEWORD_ZH_LANGUAGE,
        .ack_clip = AUDIO_ACK_CLIP_ZH_WO_ZAI,
        .available = MIMI_WAKEWORD_ZH_AVAILABLE,
        .requires_custom_model = false,
    },
    {
        .wake_word_index = MIMI_WAKEWORD_EN_WORD_INDEX,
        .wakenet_model_index = MIMI_WAKEWORD_EN_MODEL_INDEX,
        .label = MIMI_WAKEWORD_EN_LABEL,
        .language = MIMI_WAKEWORD_EN_LANGUAGE,
        .preferred_session_language = MIMI_WAKEWORD_EN_LANGUAGE,
        .ack_clip = AUDIO_ACK_CLIP_EN_IM_HERE,
        .available = MIMI_WAKEWORD_EN_AVAILABLE,
        .requires_custom_model = true,
    },
};

const wakeword_route_t *wakeword_router_resolve(int wakenet_model_index, int wake_word_index)
{
    for (int i = 0; i < (int)(sizeof(s_routes) / sizeof(s_routes[0])); i++) {
        if (s_routes[i].wakenet_model_index == wakenet_model_index &&
            s_routes[i].wake_word_index == wake_word_index) {
            return &s_routes[i];
        }
    }
    return NULL;
}

const wakeword_route_t *wakeword_router_default_route(void)
{
    return &s_routes[0];
}

int wakeword_router_route_count(void)
{
    return (int)(sizeof(s_routes) / sizeof(s_routes[0]));
}

const wakeword_route_t *wakeword_router_route_at(int index)
{
    if (index < 0 || index >= wakeword_router_route_count()) {
        return NULL;
    }
    return &s_routes[index];
}
