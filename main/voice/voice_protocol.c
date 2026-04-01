#include "voice/voice_protocol.h"
#include "mimi_config.h"
#include <stdbool.h>

static void add_common(cJSON *obj, const char *type, const char *session_id)
{
    cJSON_AddStringToObject(obj, "type", type);
    if (session_id && session_id[0]) {
        cJSON_AddStringToObject(obj, "session_id", session_id);
    }
}

cJSON *voice_protocol_create_hello(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    add_common(root, "hello", NULL);
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "transport", "websocket");

    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format", MIMI_VOICE_AUDIO_INPUT_FORMAT);
    cJSON_AddNumberToObject(audio, "sample_rate", MIMI_AUDIO_SAMPLE_RATE_HZ);
    cJSON_AddNumberToObject(audio, "channels", MIMI_AUDIO_CHANNELS);
    cJSON_AddNumberToObject(audio, "frame_duration_ms", MIMI_VOICE_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio);

    cJSON *features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "vad", true);
    cJSON_AddBoolToObject(features, "interrupt", false);
    cJSON_AddBoolToObject(features, "display_text", true);
    cJSON_AddItemToObject(root, "features", features);

    return root;
}

cJSON *voice_protocol_create_listen_start(const char *session_id, const char *language)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    add_common(root, "listen_start", session_id);
    if (language && language[0]) {
        cJSON_AddStringToObject(root, "language", language);
    }
    return root;
}

cJSON *voice_protocol_create_listen_stop(const char *session_id, const char *reason)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    add_common(root, "listen_stop", session_id);
    if (reason && reason[0]) {
        cJSON_AddStringToObject(root, "reason", reason);
    }
    return root;
}

cJSON *voice_protocol_create_tts_request(const char *session_id, const char *text, const char *language)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    add_common(root, "tts_request", session_id);
    cJSON_AddStringToObject(root, "text", text ? text : "");
    if (language && language[0]) {
        cJSON_AddStringToObject(root, "language", language);
    }
    cJSON_AddStringToObject(root, "audio_format", MIMI_VOICE_AUDIO_OUTPUT_FORMAT);
    return root;
}

cJSON *voice_protocol_create_abort(const char *session_id, const char *reason)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    add_common(root, "abort", session_id);
    if (reason && reason[0]) {
        cJSON_AddStringToObject(root, "reason", reason);
    }
    return root;
}

const char *voice_protocol_get_type(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "type");
    return cJSON_IsString(type) ? type->valuestring : NULL;
}
