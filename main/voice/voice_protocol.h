#pragma once

#include "cJSON.h"

cJSON *voice_protocol_create_hello(void);
cJSON *voice_protocol_create_listen_start(const char *session_id, const char *language);
cJSON *voice_protocol_create_listen_stop(const char *session_id, const char *reason);
cJSON *voice_protocol_create_tts_request(const char *session_id, const char *text, const char *language);
cJSON *voice_protocol_create_abort(const char *session_id, const char *reason);
const char *voice_protocol_get_type(const cJSON *root);
