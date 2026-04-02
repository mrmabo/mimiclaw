#include "tool_web_search.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

#ifndef MIMI_SECRET_TAVILY_KEYS
#define MIMI_SECRET_TAVILY_KEYS ""
#endif

static const char *TAG = "web_search";

typedef enum {
    SEARCH_PROVIDER_NONE = 0,
    SEARCH_PROVIDER_BRAVE,
    SEARCH_PROVIDER_TAVILY,
} search_provider_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} search_buf_t;

typedef struct {
    int status_code;
    bool ok;
    bool invalid_key;
    bool exhausted;
    int key_usage;
    int key_limit;
    int plan_usage;
    int plan_limit;
    char plan_name[32];
} tavily_usage_info_t;

static char s_brave_key[128] = {0};
static char s_tavily_key[MIMI_TAVILY_KEY_MAX_LEN] = {0};
static char s_tavily_keys[MIMI_TAVILY_KEY_SLOTS][MIMI_TAVILY_KEY_MAX_LEN] = {{0}};
static size_t s_tavily_key_count = 0;
static size_t s_tavily_active_index = 0;
static uint32_t s_tavily_last_check_epoch = 0;
static bool s_tavily_active_available = false;
static search_provider_t s_provider = SEARCH_PROVIDER_NONE;

#define SEARCH_BUF_SIZE     (16 * 1024)
#define SEARCH_RESULT_COUNT 5

static esp_err_t http_event_handler(esp_http_client_event_t *evt);
static void refresh_provider(void);
static void clear_tavily_runtime(void);
static void normalize_tavily_active_key(void);
static void persist_tavily_active_index(size_t index);
static void persist_tavily_check_ts(uint32_t epoch);
static void set_active_tavily_index(size_t index, bool persist);
static bool append_tavily_key(const char *value);
static void load_tavily_keys_csv(const char *csv);
static void build_tavily_config_from_sources(void);
static size_t url_encode(const char *src, char *dst, size_t dst_size);
static void format_results(cJSON *root, char *output, size_t output_size);
static void format_tavily_results(cJSON *root, char *output, size_t output_size);
static char *build_tavily_payload(const char *query);
static esp_err_t brave_search_direct(const char *url, search_buf_t *sb);
static esp_err_t brave_search_via_proxy(const char *path, search_buf_t *sb);
static esp_err_t tavily_search_direct(const char *api_key, const char *query, search_buf_t *sb, int *status_out);
static esp_err_t tavily_search_via_proxy(const char *api_key, const char *query, search_buf_t *sb, int *status_out);
static esp_err_t tavily_usage_direct(const char *api_key, search_buf_t *sb, int *status_out);
static esp_err_t tavily_usage_via_proxy(const char *api_key, search_buf_t *sb, int *status_out);
static esp_err_t tavily_fetch_usage(const char *api_key, tavily_usage_info_t *info);
static size_t append_summary(char *output, size_t output_size, size_t off, const char *fmt, ...);
static esp_err_t tavily_refresh_active_key(bool force, char *summary, size_t summary_size);
static esp_err_t maybe_run_daily_tavily_check(void);

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    search_buf_t *sb = (search_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = sb->len + evt->data_len;
        if (needed < sb->cap) {
            memcpy(sb->data + sb->len, evt->data, evt->data_len);
            sb->len += evt->data_len;
            sb->data[sb->len] = '\0';
        }
    }
    return ESP_OK;
}

static void refresh_provider(void)
{
    if (s_tavily_active_available && s_tavily_key[0] != '\0') {
        s_provider = SEARCH_PROVIDER_TAVILY;
    } else if (s_brave_key[0] != '\0') {
        s_provider = SEARCH_PROVIDER_BRAVE;
    } else {
        s_provider = SEARCH_PROVIDER_NONE;
    }
}

static void clear_tavily_runtime(void)
{
    memset(s_tavily_key, 0, sizeof(s_tavily_key));
    memset(s_tavily_keys, 0, sizeof(s_tavily_keys));
    s_tavily_key_count = 0;
    s_tavily_active_index = 0;
    s_tavily_last_check_epoch = 0;
    s_tavily_active_available = false;
}

static void normalize_tavily_active_key(void)
{
    if (s_tavily_key_count == 0) {
        s_tavily_active_index = 0;
        s_tavily_key[0] = '\0';
        s_tavily_active_available = false;
        refresh_provider();
        return;
    }

    if (s_tavily_active_index >= s_tavily_key_count) {
        s_tavily_active_index = 0;
    }

    strlcpy(s_tavily_key, s_tavily_keys[s_tavily_active_index], sizeof(s_tavily_key));
    s_tavily_active_available = (s_tavily_key[0] != '\0');
    refresh_provider();
}

static void persist_tavily_active_index(size_t index)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_u8(nvs, MIMI_NVS_KEY_TAVILY_ACTIVE, (uint8_t)index);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void persist_tavily_check_ts(uint32_t epoch)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_u32(nvs, MIMI_NVS_KEY_TAVILY_CHECK_TS, epoch);
    nvs_commit(nvs);
    nvs_close(nvs);
    s_tavily_last_check_epoch = epoch;
}

static void set_active_tavily_index(size_t index, bool persist)
{
    if (s_tavily_key_count == 0 || index >= s_tavily_key_count) {
        return;
    }

    s_tavily_active_index = index;
    normalize_tavily_active_key();
    if (persist) {
        persist_tavily_active_index(index);
    }
}

static bool append_tavily_key(const char *value)
{
    char cleaned[MIMI_TAVILY_KEY_MAX_LEN] = {0};
    size_t len = 0;

    if (!value) {
        return false;
    }

    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
        value++;
    }

    while (value[len] != '\0' &&
           value[len] != ',' &&
           value[len] != ';' &&
           value[len] != '\r' &&
           value[len] != '\n') {
        if (len + 1 >= sizeof(cleaned)) {
            break;
        }
        cleaned[len] = value[len];
        len++;
    }

    while (len > 0 && (cleaned[len - 1] == ' ' || cleaned[len - 1] == '\t')) {
        cleaned[--len] = '\0';
    }

    if (len == 0 || s_tavily_key_count >= MIMI_TAVILY_KEY_SLOTS) {
        return false;
    }

    for (size_t i = 0; i < s_tavily_key_count; i++) {
        if (strcmp(s_tavily_keys[i], cleaned) == 0) {
            return false;
        }
    }

    strlcpy(s_tavily_keys[s_tavily_key_count], cleaned, sizeof(s_tavily_keys[0]));
    s_tavily_key_count++;
    return true;
}

static void load_tavily_keys_csv(const char *csv)
{
    char blob[MIMI_TAVILY_KEYS_BLOB_MAX] = {0};
    char *cursor = NULL;
    char *token = NULL;

    if (!csv || csv[0] == '\0') {
        return;
    }

    strlcpy(blob, csv, sizeof(blob));
    token = strtok_r(blob, ",;\r\n", &cursor);
    while (token) {
        append_tavily_key(token);
        token = strtok_r(NULL, ",;\r\n", &cursor);
    }
}

static void build_tavily_config_from_sources(void)
{
    clear_tavily_runtime();

    if (MIMI_SECRET_TAVILY_KEY[0] != '\0') {
        append_tavily_key(MIMI_SECRET_TAVILY_KEY);
    }
    if (MIMI_SECRET_TAVILY_KEYS[0] != '\0') {
        load_tavily_keys_csv(MIMI_SECRET_TAVILY_KEYS);
    }

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs) == ESP_OK) {
        char single[MIMI_TAVILY_KEY_MAX_LEN] = {0};
        char multi[MIMI_TAVILY_KEYS_BLOB_MAX] = {0};
        size_t len = sizeof(single);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_TAVILY_KEY, single, &len) == ESP_OK && single[0] != '\0') {
            clear_tavily_runtime();
            append_tavily_key(single);
        }

        len = sizeof(multi);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_TAVILY_KEYS, multi, &len) == ESP_OK && multi[0] != '\0') {
            clear_tavily_runtime();
            load_tavily_keys_csv(multi);
        }

        uint8_t active = 0;
        if (nvs_get_u8(nvs, MIMI_NVS_KEY_TAVILY_ACTIVE, &active) == ESP_OK) {
            s_tavily_active_index = active;
        }

        uint32_t last_check = 0;
        if (nvs_get_u32(nvs, MIMI_NVS_KEY_TAVILY_CHECK_TS, &last_check) == ESP_OK) {
            s_tavily_last_check_epoch = last_check;
        }

        nvs_close(nvs);
    }

    normalize_tavily_active_key();
}

esp_err_t tool_web_search_init(void)
{
    memset(s_brave_key, 0, sizeof(s_brave_key));
    if (MIMI_SECRET_SEARCH_KEY[0] != '\0') {
        strlcpy(s_brave_key, MIMI_SECRET_SEARCH_KEY, sizeof(s_brave_key));
    }

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs) == ESP_OK) {
        char brave[sizeof(s_brave_key)] = {0};
        size_t len = sizeof(brave);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, brave, &len) == ESP_OK && brave[0] != '\0') {
            strlcpy(s_brave_key, brave, sizeof(s_brave_key));
        }
        nvs_close(nvs);
    }

    build_tavily_config_from_sources();
    refresh_provider();

    if (s_provider == SEARCH_PROVIDER_TAVILY) {
        ESP_LOGI(TAG, "Web search initialized (provider=tavily, keys=%d, active=%d)",
                 (int)s_tavily_key_count, (int)s_tavily_active_index);
    } else if (s_provider == SEARCH_PROVIDER_BRAVE) {
        ESP_LOGI(TAG, "Web search initialized (provider=brave)");
    } else {
        ESP_LOGW(TAG, "No search API key. Use CLI: set_search_key, set_tavily_key, or set_tavily_keys");
    }
    return ESP_OK;
}

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; *src && pos < dst_size - 3; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

static void format_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *web = cJSON_GetObjectItem(root, "web");
    if (!web) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    cJSON *results = cJSON_GetObjectItem(web, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT || off >= output_size - 1) {
            break;
        }

        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *url = cJSON_GetObjectItem(item, "url");
        cJSON *desc = cJSON_GetObjectItem(item, "description");

        int written = snprintf(output + off, output_size - off,
                               "%d. %s\n   %s\n   %s\n\n",
                               idx + 1,
                               (title && cJSON_IsString(title)) ? title->valuestring : "(no title)",
                               (url && cJSON_IsString(url)) ? url->valuestring : "",
                               (desc && cJSON_IsString(desc)) ? desc->valuestring : "");
        if (written < 0) {
            break;
        }
        if ((size_t)written >= output_size - off) {
            off = output_size - 1;
            break;
        }
        off += (size_t)written;
        idx++;
    }
}

static void format_tavily_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT || off >= output_size - 1) {
            break;
        }

        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *url = cJSON_GetObjectItem(item, "url");
        cJSON *content = cJSON_GetObjectItem(item, "content");

        int written = snprintf(output + off, output_size - off,
                               "%d. %s\n   %s\n   %s\n\n",
                               idx + 1,
                               (title && cJSON_IsString(title)) ? title->valuestring : "(no title)",
                               (url && cJSON_IsString(url)) ? url->valuestring : "",
                               (content && cJSON_IsString(content)) ? content->valuestring : "");
        if (written < 0) {
            break;
        }
        if ((size_t)written >= output_size - off) {
            off = output_size - 1;
            break;
        }
        off += (size_t)written;
        idx++;
    }
}

static char *build_tavily_payload(const char *query)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "query", query);
    cJSON_AddNumberToObject(root, "max_results", SEARCH_RESULT_COUNT);
    cJSON_AddBoolToObject(root, "include_answer", false);
    cJSON_AddStringToObject(root, "search_depth", "basic");

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static esp_err_t brave_search_direct(const char *url, search_buf_t *sb)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Subscription-Token", s_brave_key);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Brave API returned %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t brave_search_via_proxy(const char *path, search_buf_t *sb)
{
    proxy_conn_t *conn = proxy_conn_open("api.search.brave.com", 443, 15000);
    if (!conn) {
        return ESP_ERR_HTTP_CONNECT;
    }

    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "GET %s HTTP/1.1\r\n"
                        "Host: api.search.brave.com\r\n"
                        "Accept: application/json\r\n"
                        "X-Subscription-Token: %s\r\n"
                        "Connection: close\r\n\r\n",
                        path, s_brave_key);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) {
            break;
        }
        size_t copy = (total + (size_t)n < sb->cap - 1) ? (size_t)n : (sb->cap - 1 - total);
        if (copy > 0) {
            memcpy(sb->data + total, tmp, copy);
            total += copy;
        }
    }
    sb->data[total] = '\0';
    sb->len = total;
    proxy_conn_close(conn);

    int status = 0;
    if (total > 5 && strncmp(sb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(sb->data, ' ');
        if (sp) {
            status = atoi(sp + 1);
        }
    }

    char *body = strstr(sb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = total - (size_t)(body - sb->data);
        memmove(sb->data, body, blen);
        sb->len = blen;
        sb->data[sb->len] = '\0';
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Brave API returned %d via proxy", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t tavily_search_direct(const char *api_key, const char *query, search_buf_t *sb, int *status_out)
{
    char *payload = build_tavily_payload(query);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = "https://api.tavily.com/search",
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(payload);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");

    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (status_out) {
        *status_out = status;
    }

    esp_http_client_cleanup(client);
    free(payload);

    if (err != ESP_OK) {
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "Tavily search returned %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t tavily_search_via_proxy(const char *api_key, const char *query, search_buf_t *sb, int *status_out)
{
    proxy_conn_t *conn = proxy_conn_open("api.tavily.com", 443, 15000);
    if (!conn) {
        return ESP_ERR_HTTP_CONNECT;
    }

    char *payload = build_tavily_payload(query);
    if (!payload) {
        proxy_conn_close(conn);
        return ESP_ERR_NO_MEM;
    }

    char header[768];
    int hlen = snprintf(header, sizeof(header),
                        "POST /search HTTP/1.1\r\n"
                        "Host: api.tavily.com\r\n"
                        "Accept: application/json\r\n"
                        "Content-Type: application/json\r\n"
                        "Authorization: Bearer %s\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n\r\n",
                        api_key, (int)strlen(payload));

    if (proxy_conn_write(conn, header, hlen) < 0 ||
        proxy_conn_write(conn, payload, strlen(payload)) < 0) {
        free(payload);
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }
    free(payload);

    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) {
            break;
        }
        size_t copy = (total + (size_t)n < sb->cap - 1) ? (size_t)n : (sb->cap - 1 - total);
        if (copy > 0) {
            memcpy(sb->data + total, tmp, copy);
            total += copy;
        }
    }
    sb->data[total] = '\0';
    sb->len = total;
    proxy_conn_close(conn);

    int status = 0;
    if (total > 5 && strncmp(sb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(sb->data, ' ');
        if (sp) {
            status = atoi(sp + 1);
        }
    }
    if (status_out) {
        *status_out = status;
    }

    char *body = strstr(sb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = total - (size_t)(body - sb->data);
        memmove(sb->data, body, blen);
        sb->len = blen;
        sb->data[sb->len] = '\0';
    }

    if (status != 200) {
        ESP_LOGW(TAG, "Tavily search returned %d via proxy", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t tavily_usage_direct(const char *api_key, search_buf_t *sb, int *status_out)
{
    esp_http_client_config_t config = {
        .url = "https://api.tavily.com/usage",
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (status_out) {
        *status_out = status;
    }

    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }
    if (status != 200) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t tavily_usage_via_proxy(const char *api_key, search_buf_t *sb, int *status_out)
{
    proxy_conn_t *conn = proxy_conn_open("api.tavily.com", 443, 15000);
    if (!conn) {
        return ESP_ERR_HTTP_CONNECT;
    }

    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "GET /usage HTTP/1.1\r\n"
                        "Host: api.tavily.com\r\n"
                        "Accept: application/json\r\n"
                        "Authorization: Bearer %s\r\n"
                        "Connection: close\r\n\r\n",
                        api_key);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) {
            break;
        }
        size_t copy = (total + (size_t)n < sb->cap - 1) ? (size_t)n : (sb->cap - 1 - total);
        if (copy > 0) {
            memcpy(sb->data + total, tmp, copy);
            total += copy;
        }
    }
    sb->data[total] = '\0';
    sb->len = total;
    proxy_conn_close(conn);

    int status = 0;
    if (total > 5 && strncmp(sb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(sb->data, ' ');
        if (sp) {
            status = atoi(sp + 1);
        }
    }
    if (status_out) {
        *status_out = status;
    }

    char *body = strstr(sb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = total - (size_t)(body - sb->data);
        memmove(sb->data, body, blen);
        sb->len = blen;
        sb->data[sb->len] = '\0';
    }

    if (status != 200) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t tavily_fetch_usage(const char *api_key, tavily_usage_info_t *info)
{
    search_buf_t sb = {0};
    cJSON *root = NULL;

    memset(info, 0, sizeof(*info));

    sb.data = heap_caps_calloc(1, SEARCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!sb.data) {
        return ESP_ERR_NO_MEM;
    }
    sb.cap = SEARCH_BUF_SIZE;

    esp_err_t err;
    int status = 0;
    if (http_proxy_is_enabled()) {
        err = tavily_usage_via_proxy(api_key, &sb, &status);
    } else {
        err = tavily_usage_direct(api_key, &sb, &status);
    }
    info->status_code = status;

    if (err != ESP_OK) {
        info->invalid_key = (status == 401 || status == 403);
        free(sb.data);
        return err;
    }

    root = cJSON_Parse(sb.data);
    free(sb.data);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *key = cJSON_GetObjectItem(root, "key");
    cJSON *account = cJSON_GetObjectItem(root, "account");
    if (key && cJSON_IsObject(key)) {
        cJSON *usage = cJSON_GetObjectItem(key, "usage");
        cJSON *limit = cJSON_GetObjectItem(key, "limit");
        if (usage && cJSON_IsNumber(usage)) {
            info->key_usage = (int)usage->valuedouble;
        }
        if (limit && cJSON_IsNumber(limit)) {
            info->key_limit = (int)limit->valuedouble;
        }
    }
    if (account && cJSON_IsObject(account)) {
        cJSON *plan_usage = cJSON_GetObjectItem(account, "plan_usage");
        cJSON *plan_limit = cJSON_GetObjectItem(account, "plan_limit");
        cJSON *plan_name = cJSON_GetObjectItem(account, "current_plan");
        if (plan_usage && cJSON_IsNumber(plan_usage)) {
            info->plan_usage = (int)plan_usage->valuedouble;
        }
        if (plan_limit && cJSON_IsNumber(plan_limit)) {
            info->plan_limit = (int)plan_limit->valuedouble;
        }
        if (plan_name && cJSON_IsString(plan_name)) {
            strlcpy(info->plan_name, plan_name->valuestring, sizeof(info->plan_name));
        }
    }

    info->ok = true;
    info->exhausted =
        ((info->key_limit > 0) && (info->key_usage >= info->key_limit)) ||
        ((info->plan_limit > 0) && (info->plan_usage >= info->plan_limit));

    cJSON_Delete(root);
    return ESP_OK;
}

static size_t append_summary(char *output, size_t output_size, size_t off, const char *fmt, ...)
{
    va_list args;
    int written;

    if (!output || off >= output_size) {
        return off;
    }

    va_start(args, fmt);
    written = vsnprintf(output + off, output_size - off, fmt, args);
    va_end(args);

    if (written < 0) {
        return off;
    }
    if ((size_t)written >= output_size - off) {
        return output_size - 1;
    }
    return off + (size_t)written;
}

static esp_err_t tavily_refresh_active_key(bool force, char *summary, size_t summary_size)
{
    time_t now = time(NULL);
    bool have_success = false;
    bool current_known_unusable = false;
    int fallback_unknown_index = -1;
    size_t summary_off = 0;
    size_t original_index = s_tavily_active_index;

    if (summary && summary_size > 0) {
        summary[0] = '\0';
    }

    if (s_tavily_key_count == 0) {
        if (summary) {
            snprintf(summary, summary_size, "No Tavily keys configured.");
        }
        refresh_provider();
        return ESP_ERR_NOT_FOUND;
    }

    if (!force && s_tavily_last_check_epoch != 0 && now > 0 &&
        ((uint32_t)now - s_tavily_last_check_epoch) < MIMI_TAVILY_CHECK_INTERVAL_S) {
        if (summary) {
            snprintf(summary, summary_size,
                     "Tavily check skipped; last check was %lu seconds ago. Active key: %d/%d.",
                     (unsigned long)((uint32_t)now - s_tavily_last_check_epoch),
                     (int)(s_tavily_active_index + 1), (int)s_tavily_key_count);
        }
        return ESP_OK;
    }

    summary_off = append_summary(summary, summary_size, summary_off, "Tavily credits check:\n");

    for (size_t offset = 0; offset < s_tavily_key_count; offset++) {
        size_t idx = (original_index + offset) % s_tavily_key_count;
        tavily_usage_info_t info;
        esp_err_t err = tavily_fetch_usage(s_tavily_keys[idx], &info);

        if (err == ESP_OK && info.ok) {
            have_success = true;
            summary_off = append_summary(
                summary, summary_size, summary_off,
                "- key %d/%d: key=%d/%d, plan=%d/%d%s%s\n",
                (int)(idx + 1), (int)s_tavily_key_count,
                info.key_usage, info.key_limit,
                info.plan_usage, info.plan_limit,
                info.plan_name[0] ? " plan=" : "",
                info.plan_name[0] ? info.plan_name : "");

            if (!info.exhausted) {
                set_active_tavily_index(idx, true);
                s_tavily_active_available = true;
                refresh_provider();
                if (now > 0) {
                    persist_tavily_check_ts((uint32_t)now);
                }
                if (summary) {
                    append_summary(summary, summary_size, summary_off,
                                   "Active Tavily key: %d/%d.\n",
                                   (int)(s_tavily_active_index + 1), (int)s_tavily_key_count);
                }
                return ESP_OK;
            }

            if (idx == original_index) {
                current_known_unusable = true;
            }
        } else {
            if ((info.status_code == 401 || info.status_code == 403) && idx == original_index) {
                current_known_unusable = true;
            }
            if (fallback_unknown_index < 0 && idx != original_index) {
                fallback_unknown_index = (int)idx;
            }
            summary_off = append_summary(
                summary, summary_size, summary_off,
                "- key %d/%d: usage check failed (status=%d, err=%s)\n",
                (int)(idx + 1), (int)s_tavily_key_count,
                info.status_code, esp_err_to_name(err));
        }
    }

    if (current_known_unusable && fallback_unknown_index >= 0) {
        set_active_tavily_index((size_t)fallback_unknown_index, true);
        s_tavily_active_available = true;
        refresh_provider();
        if (now > 0) {
            persist_tavily_check_ts((uint32_t)now);
        }
        if (summary) {
            append_summary(summary, summary_size, summary_off,
                           "Active key moved to %d/%d as a best-effort fallback.\n",
                           fallback_unknown_index + 1, (int)s_tavily_key_count);
        }
        return ESP_OK;
    }

    if (have_success) {
        s_tavily_active_available = false;
        s_tavily_key[0] = '\0';
        refresh_provider();
        if (now > 0) {
            persist_tavily_check_ts((uint32_t)now);
        }
        if (summary) {
            append_summary(summary, summary_size, summary_off,
                           "All checked Tavily keys are exhausted or unavailable. Active provider: %s.\n",
                           s_provider == SEARCH_PROVIDER_BRAVE ? "brave" : "none");
        }
        return (s_provider == SEARCH_PROVIDER_BRAVE) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    if (summary) {
        append_summary(summary, summary_size, summary_off,
                       "Could not verify Tavily usage; keeping current active key %d/%d.\n",
                       (int)(s_tavily_active_index + 1), (int)s_tavily_key_count);
    }
    return ESP_OK;
}

static esp_err_t maybe_run_daily_tavily_check(void)
{
    if (s_tavily_key_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return tavily_refresh_active_key(false, NULL, 0);
}

esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = NULL;
    cJSON *query = NULL;
    search_buf_t sb = {0};
    char encoded_query[256];
    char query_copy[256];
    esp_err_t err = ESP_OK;
    int tavily_status = 0;
    bool used_tavily = false;

    maybe_run_daily_tavily_check();

    if (s_provider == SEARCH_PROVIDER_NONE) {
        snprintf(output, output_size,
                 "Error: No available search provider configured. Set MIMI_SECRET_TAVILY_KEY, "
                 "MIMI_SECRET_TAVILY_KEYS, or MIMI_SECRET_SEARCH_KEY.");
        return ESP_ERR_INVALID_STATE;
    }

    input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    query = cJSON_GetObjectItem(input, "query");
    if (!query || !cJSON_IsString(query) || query->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Searching: %s", query->valuestring);

    url_encode(query->valuestring, encoded_query, sizeof(encoded_query));
    snprintf(query_copy, sizeof(query_copy), "%s", query->valuestring);
    cJSON_Delete(input);

    sb.data = heap_caps_calloc(1, SEARCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!sb.data) {
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }
    sb.cap = SEARCH_BUF_SIZE;

    if (s_provider == SEARCH_PROVIDER_TAVILY) {
        search_provider_t previous_provider = s_provider;
        char previous_key[MIMI_TAVILY_KEY_MAX_LEN] = {0};

        used_tavily = true;
        strlcpy(previous_key, s_tavily_key, sizeof(previous_key));
        if (http_proxy_is_enabled()) {
            err = tavily_search_via_proxy(s_tavily_key, query_copy, &sb, &tavily_status);
        } else {
            err = tavily_search_direct(s_tavily_key, query_copy, &sb, &tavily_status);
        }

        if (err != ESP_OK) {
            tavily_refresh_active_key(true, NULL, 0);
            if (s_provider == SEARCH_PROVIDER_TAVILY &&
                strcmp(previous_key, s_tavily_key) != 0) {
                memset(sb.data, 0, SEARCH_BUF_SIZE);
                sb.len = 0;
                if (http_proxy_is_enabled()) {
                    err = tavily_search_via_proxy(s_tavily_key, query_copy, &sb, &tavily_status);
                } else {
                    err = tavily_search_direct(s_tavily_key, query_copy, &sb, &tavily_status);
                }
            } else if (previous_provider != s_provider && s_provider == SEARCH_PROVIDER_BRAVE) {
                char path[384];
                snprintf(path, sizeof(path),
                         "/res/v1/web/search?q=%s&count=%d", encoded_query, SEARCH_RESULT_COUNT);
                memset(sb.data, 0, SEARCH_BUF_SIZE);
                sb.len = 0;
                used_tavily = false;
                if (http_proxy_is_enabled()) {
                    err = brave_search_via_proxy(path, &sb);
                } else {
                    char url[512];
                    snprintf(url, sizeof(url), "https://api.search.brave.com%s", path);
                    err = brave_search_direct(url, &sb);
                }
            }
        }
    } else {
        char path[384];
        snprintf(path, sizeof(path),
                 "/res/v1/web/search?q=%s&count=%d", encoded_query, SEARCH_RESULT_COUNT);
        if (http_proxy_is_enabled()) {
            err = brave_search_via_proxy(path, &sb);
        } else {
            char url[512];
            snprintf(url, sizeof(url), "https://api.search.brave.com%s", path);
            err = brave_search_direct(url, &sb);
        }
    }

    if (err != ESP_OK) {
        free(sb.data);
        if (used_tavily && (tavily_status == 401 || tavily_status == 403 || tavily_status == 429)) {
            snprintf(output, output_size, "Error: Search request failed after Tavily key rotation attempt.");
        } else {
            snprintf(output, output_size, "Error: Search request failed");
        }
        return err;
    }

    cJSON *root = cJSON_Parse(sb.data);
    free(sb.data);
    if (!root) {
        snprintf(output, output_size, "Error: Failed to parse search results");
        return ESP_FAIL;
    }

    if (s_provider == SEARCH_PROVIDER_TAVILY) {
        format_tavily_results(root, output, output_size);
    } else {
        format_results(root, output, output_size);
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Search complete, %d bytes result", (int)strlen(output));
    return ESP_OK;
}

esp_err_t tool_web_search_set_key(const char *api_key)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(s_brave_key, api_key, sizeof(s_brave_key));
    refresh_provider();
    ESP_LOGI(TAG, "Brave search API key saved");
    return ESP_OK;
}

esp_err_t tool_web_search_set_tavily_key(const char *api_key)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, MIMI_NVS_KEY_TAVILY_KEY, api_key);
    if (err == ESP_OK) {
        nvs_erase_key(nvs, MIMI_NVS_KEY_TAVILY_KEYS);
        nvs_set_u8(nvs, MIMI_NVS_KEY_TAVILY_ACTIVE, 0);
        nvs_set_u32(nvs, MIMI_NVS_KEY_TAVILY_CHECK_TS, 0);
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    tool_web_search_init();
    ESP_LOGI(TAG, "Single Tavily API key saved");
    return ESP_OK;
}

esp_err_t tool_web_search_set_tavily_keys(const char *api_keys_csv)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, MIMI_NVS_KEY_TAVILY_KEYS, api_keys_csv);
    if (err == ESP_OK) {
        nvs_erase_key(nvs, MIMI_NVS_KEY_TAVILY_KEY);
        nvs_set_u8(nvs, MIMI_NVS_KEY_TAVILY_ACTIVE, 0);
        nvs_set_u32(nvs, MIMI_NVS_KEY_TAVILY_CHECK_TS, 0);
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    tool_web_search_init();
    ESP_LOGI(TAG, "Tavily API key list saved");
    return ESP_OK;
}

esp_err_t tool_tavily_check_credits_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = NULL;
    cJSON *force = NULL;
    bool force_check = true;
    esp_err_t err;

    if (input_json && input_json[0] != '\0') {
        input = cJSON_Parse(input_json);
        if (!input) {
            snprintf(output, output_size, "Error: Invalid input JSON");
            return ESP_ERR_INVALID_ARG;
        }
        force = cJSON_GetObjectItem(input, "force");
        if (force && cJSON_IsBool(force)) {
            force_check = cJSON_IsTrue(force);
        }
    }

    err = tavily_refresh_active_key(force_check, output, output_size);
    if (input) {
        cJSON_Delete(input);
    }
    return err;
}
