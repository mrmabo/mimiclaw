#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

/* Channel identifiers */
#define MIMI_CHAN_TELEGRAM   "telegram"
#define MIMI_CHAN_FEISHU     "feishu"
#define MIMI_CHAN_WEBSOCKET  "websocket"
#define MIMI_CHAN_CLI        "cli"
#define MIMI_CHAN_SYSTEM     "system"
#define MIMI_CHAN_VOICE      "voice"

typedef enum {
    MIMI_MSG_TYPE_TEXT = 0,
    MIMI_MSG_TYPE_VOICE_TEXT_PARTIAL,
    MIMI_MSG_TYPE_VOICE_TEXT_FINAL,
    MIMI_MSG_TYPE_AUDIO_START,
    MIMI_MSG_TYPE_AUDIO_CHUNK,
    MIMI_MSG_TYPE_AUDIO_STOP,
    MIMI_MSG_TYPE_CONTROL,
} mimi_msg_type_t;

#define MIMI_MSG_FLAG_FINAL        (1U << 0)
#define MIMI_MSG_FLAG_PARTIAL      (1U << 1)
#define MIMI_MSG_FLAG_FROM_USER    (1U << 2)
#define MIMI_MSG_FLAG_FROM_AGENT   (1U << 3)

/* Message types on the bus */
typedef struct {
    char channel[16];       /* "telegram", "websocket", "cli" */
    char chat_id[96];       /* Telegram/Feishu chat_id, open_id, or WS client id */
    char session_id[64];    /* Voice / multimodal session identifier */
    char preferred_language[16];
    char source_role[16];   /* "user", "assistant", "system", "gateway" */
    mimi_msg_type_t msg_type;
    uint32_t flags;
    uint32_t seq;
    int64_t timestamp_ms;
    char *content;          /* Heap-allocated message text (caller must free) */
} mimi_msg_t;

/**
 * Initialize a message with zero/default metadata.
 */
void message_bus_init_msg(mimi_msg_t *msg);

/**
 * Initialize the message bus (inbound + outbound FreeRTOS queues).
 */
esp_err_t message_bus_init(void);

/**
 * Push a message to the inbound queue (towards Agent Loop).
 * The bus takes ownership of msg->content.
 */
esp_err_t message_bus_push_inbound(const mimi_msg_t *msg);

/**
 * Pop a message from the inbound queue (blocking).
 * Caller must free msg->content when done.
 */
esp_err_t message_bus_pop_inbound(mimi_msg_t *msg, uint32_t timeout_ms);

/**
 * Push a message to the outbound queue (towards channels).
 * The bus takes ownership of msg->content.
 */
esp_err_t message_bus_push_outbound(const mimi_msg_t *msg);

/**
 * Pop a message from the outbound queue (blocking).
 * Caller must free msg->content when done.
 */
esp_err_t message_bus_pop_outbound(mimi_msg_t *msg, uint32_t timeout_ms);
