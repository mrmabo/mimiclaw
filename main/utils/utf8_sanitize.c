#include "utils/utf8_sanitize.h"

#include <stdlib.h>
#include <string.h>

static size_t utf8_valid_sequence_len(const unsigned char *s, size_t remaining)
{
    if (!s || remaining == 0) {
        return 0;
    }

    if (s[0] <= 0x7F) {
        return 1;
    }

    if (remaining >= 2 &&
        s[0] >= 0xC2 && s[0] <= 0xDF &&
        s[1] >= 0x80 && s[1] <= 0xBF) {
        return 2;
    }

    if (remaining >= 3) {
        if (s[0] == 0xE0 &&
            s[1] >= 0xA0 && s[1] <= 0xBF &&
            s[2] >= 0x80 && s[2] <= 0xBF) {
            return 3;
        }
        if (s[0] >= 0xE1 && s[0] <= 0xEC &&
            s[1] >= 0x80 && s[1] <= 0xBF &&
            s[2] >= 0x80 && s[2] <= 0xBF) {
            return 3;
        }
        if (s[0] == 0xED &&
            s[1] >= 0x80 && s[1] <= 0x9F &&
            s[2] >= 0x80 && s[2] <= 0xBF) {
            return 3;
        }
        if (s[0] >= 0xEE && s[0] <= 0xEF &&
            s[1] >= 0x80 && s[1] <= 0xBF &&
            s[2] >= 0x80 && s[2] <= 0xBF) {
            return 3;
        }
    }

    if (remaining >= 4) {
        if (s[0] == 0xF0 &&
            s[1] >= 0x90 && s[1] <= 0xBF &&
            s[2] >= 0x80 && s[2] <= 0xBF &&
            s[3] >= 0x80 && s[3] <= 0xBF) {
            return 4;
        }
        if (s[0] >= 0xF1 && s[0] <= 0xF3 &&
            s[1] >= 0x80 && s[1] <= 0xBF &&
            s[2] >= 0x80 && s[2] <= 0xBF &&
            s[3] >= 0x80 && s[3] <= 0xBF) {
            return 4;
        }
        if (s[0] == 0xF4 &&
            s[1] >= 0x80 && s[1] <= 0x8F &&
            s[2] >= 0x80 && s[2] <= 0xBF &&
            s[3] >= 0x80 && s[3] <= 0xBF) {
            return 4;
        }
    }

    return 0;
}

char *utf8_sanitize_dup(const char *input, size_t *replaced_count)
{
    static const unsigned char kReplacement[] = {0xEF, 0xBF, 0xBD};

    if (replaced_count) {
        *replaced_count = 0;
    }
    if (!input) {
        return NULL;
    }

    size_t in_len = strlen(input);
    char *out = malloc((in_len * 3) + 1);
    if (!out) {
        return NULL;
    }

    size_t in_off = 0;
    size_t out_off = 0;
    size_t replaced = 0;

    while (in_off < in_len) {
        size_t seq_len = utf8_valid_sequence_len((const unsigned char *)input + in_off,
                                                 in_len - in_off);
        if (seq_len > 0) {
            memcpy(out + out_off, input + in_off, seq_len);
            out_off += seq_len;
            in_off += seq_len;
            continue;
        }

        memcpy(out + out_off, kReplacement, sizeof(kReplacement));
        out_off += sizeof(kReplacement);
        in_off++;
        replaced++;
    }

    out[out_off] = '\0';
    if (replaced_count) {
        *replaced_count = replaced;
    }
    return out;
}

size_t utf8_sanitize_cjson_strings(cJSON *item)
{
    size_t replaced_total = 0;

    for (cJSON *cur = item; cur; cur = cur->next) {
        if (cJSON_IsString(cur) && cur->valuestring) {
            size_t replaced = 0;
            char *sanitized = utf8_sanitize_dup(cur->valuestring, &replaced);
            if (sanitized) {
                if (replaced > 0) {
                    free(cur->valuestring);
                    cur->valuestring = sanitized;
                    replaced_total += replaced;
                } else {
                    free(sanitized);
                }
            }
        }

        if (cur->child) {
            replaced_total += utf8_sanitize_cjson_strings(cur->child);
        }
    }

    return replaced_total;
}
