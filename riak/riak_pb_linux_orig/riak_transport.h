#ifndef RIAK_PB_TRANSPORT_H
#define RIAK_PB_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RIAK_MAX_FRAME_SIZE ((size_t) 64 * 1024 * 1024)

typedef struct {
    uint8_t code;
    uint8_t *data;
    size_t length;
} riak_reply;

/* A fresh connection per command avoids carrying a partially read frame forward. */
bool riak_exchange(const char *host, unsigned int port, unsigned int timeout_ms,
    uint8_t code, const uint8_t *payload, size_t length, riak_reply *reply,
    char *error, size_t error_size);
void riak_reply_free(riak_reply *reply);

#endif
