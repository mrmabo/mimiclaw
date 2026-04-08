#pragma once

#include <stddef.h>
#include "cJSON.h"

/* Duplicate a string and replace invalid UTF-8 bytes with U+FFFD. */
char *utf8_sanitize_dup(const char *input, size_t *replaced_count);

/* Recursively sanitize all string values in a cJSON tree. */
size_t utf8_sanitize_cjson_strings(cJSON *item);
