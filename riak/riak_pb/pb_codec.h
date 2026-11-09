#ifndef RIAK_PB_CODEC_H
#define RIAK_PB_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PB_MAX_BUFFER_SIZE ((size_t) 128 * 1024 * 1024)
#define PB_MAX_FIELD_NUMBER UINT32_C(0x1fffffff)

typedef struct {
    uint8_t *data;
    size_t len;
    size_t capacity;
} pb_buffer;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t offset;
} pb_reader;

typedef struct {
    uint32_t number;
    uint8_t wire;
    uint64_t integer;
    const uint8_t *bytes;
    size_t length;
} pb_field;

void pb_buffer_init(pb_buffer *buffer);
void pb_buffer_free(pb_buffer *buffer);

/* False leaves the encoded message unchanged. Content may contain NUL bytes. */
bool pb_add_bytes(pb_buffer *buffer, uint32_t number, const void *data, size_t length);
bool pb_add_uint(pb_buffer *buffer, uint32_t number, uint64_t value);

void pb_reader_init(pb_reader *reader, const void *data, size_t length);

/*
 * Return 1 for a field, 0 at end, or -1 for malformed/oversized input.
 * An error leaves reader and field unchanged. Groups (wire 3/4) are rejected.
 * Wire 0 uses integer. Wire 1/2/5 use borrowed bytes and length; fixed fields
 * have lengths 8/4. Borrowed pointers remain valid only while input is alive.
 */
int pb_next(pb_reader *reader, pb_field *field);

#endif
