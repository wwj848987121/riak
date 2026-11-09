#include "pb_codec.h"

#include <stdlib.h>
#include <string.h>

static size_t pb_varint_size(uint64_t value)
{
    size_t length = 1;

    while (value >= UINT64_C(0x80)) {
        value >>= 7;
        length++;
    }
    return length;
}

static size_t pb_write_varint(uint8_t *destination, uint64_t value)
{
    size_t length = 0;

    do {
        uint8_t byte = (uint8_t) (value & UINT64_C(0x7f));
        value >>= 7;
        destination[length++] = (uint8_t) (byte | (value != 0 ? 0x80 : 0));
    } while (value != 0);

    return length;
}

/* Keep the old allocation alive until append finishes, including aliased input. */
static uint8_t *pb_prepare_append(const pb_buffer *buffer, size_t extra, size_t *capacity)
{
    uint8_t *storage;
    size_t required;

    if (buffer == NULL || buffer->len > buffer->capacity
        || buffer->capacity > PB_MAX_BUFFER_SIZE
        || (buffer->capacity != 0 && buffer->data == NULL)
        || extra > PB_MAX_BUFFER_SIZE - buffer->len) {
        return NULL;
    }

    required = buffer->len + extra;
    *capacity = buffer->capacity;
    if (required <= buffer->capacity) {
        return buffer->data;
    }

    *capacity = buffer->capacity != 0 ? buffer->capacity : 64;
    while (*capacity < required) {
        if (*capacity > PB_MAX_BUFFER_SIZE / 2) {
            *capacity = PB_MAX_BUFFER_SIZE;
        } else {
            *capacity *= 2;
        }
    }

    storage = malloc(*capacity);
    if (storage != NULL && buffer->len != 0) {
        memcpy(storage, buffer->data, buffer->len);
    }
    return storage;
}

static void pb_commit_append(pb_buffer *buffer, uint8_t *storage, size_t capacity, size_t extra)
{
    if (storage != buffer->data) {
        free(buffer->data);
    }
    buffer->data = storage;
    buffer->capacity = capacity;
    buffer->len += extra;
}

void pb_buffer_init(pb_buffer *buffer)
{
    if (buffer != NULL) {
        buffer->data = NULL;
        buffer->len = 0;
        buffer->capacity = 0;
    }
}

void pb_buffer_free(pb_buffer *buffer)
{
    if (buffer != NULL) {
        free(buffer->data);
        pb_buffer_init(buffer);
    }
}

bool pb_add_bytes(pb_buffer *buffer, uint32_t number, const void *data, size_t length)
{
    uint64_t tag;
    size_t tag_length;
    size_t header_length;
    size_t extra;
    size_t capacity;
    uint8_t *storage;

    if (number == 0 || number > PB_MAX_FIELD_NUMBER
        || (length != 0 && data == NULL) || length > PB_MAX_BUFFER_SIZE) {
        return false;
    }

    tag = ((uint64_t) number << 3) | UINT64_C(2);
    tag_length = pb_varint_size(tag);
    header_length = tag_length + pb_varint_size((uint64_t) length);
    if (length > PB_MAX_BUFFER_SIZE - header_length) {
        return false;
    }
    extra = header_length + length;
    storage = pb_prepare_append(buffer, extra, &capacity);
    if (storage == NULL) {
        return false;
    }

    /* Copy before writing the header so existing buffer storage may be input. */
    if (length != 0) {
        memmove(storage + buffer->len + header_length, data, length);
    }
    pb_write_varint(storage + buffer->len, tag);
    pb_write_varint(storage + buffer->len + tag_length, (uint64_t) length);
    pb_commit_append(buffer, storage, capacity, extra);
    return true;
}

bool pb_add_uint(pb_buffer *buffer, uint32_t number, uint64_t value)
{
    uint64_t tag;
    size_t tag_length;
    size_t extra;
    size_t capacity;
    uint8_t *storage;

    if (number == 0 || number > PB_MAX_FIELD_NUMBER) {
        return false;
    }

    tag = (uint64_t) number << 3;
    tag_length = pb_varint_size(tag);
    extra = tag_length + pb_varint_size(value);
    storage = pb_prepare_append(buffer, extra, &capacity);
    if (storage == NULL) {
        return false;
    }

    pb_write_varint(storage + buffer->len, tag);
    pb_write_varint(storage + buffer->len + tag_length, value);
    pb_commit_append(buffer, storage, capacity, extra);
    return true;
}

void pb_reader_init(pb_reader *reader, const void *data, size_t length)
{
    if (reader != NULL) {
        reader->data = data;
        reader->len = length;
        reader->offset = 0;
    }
}

static bool pb_read_varint(pb_reader *reader, uint64_t *value)
{
    uint64_t result = 0;
    unsigned int index;

    for (index = 0; index < 10; index++) {
        uint8_t byte;

        if (reader->offset >= reader->len) {
            return false;
        }
        byte = reader->data[reader->offset++];
        /* The tenth byte can only contribute bit 63 and cannot continue. */
        if (index == 9 && (byte & 0xfe) != 0) {
            return false;
        }
        result |= (uint64_t) (byte & 0x7f) << (index * 7);
        if ((byte & 0x80) == 0) {
            *value = result;
            return true;
        }
    }
    return false;
}

int pb_next(pb_reader *reader, pb_field *field)
{
    pb_reader cursor;
    pb_field result = {0};
    uint64_t tag;
    uint64_t length;
    uint64_t number;

    if (reader == NULL || field == NULL || reader->len > PB_MAX_BUFFER_SIZE
        || reader->offset > reader->len
        || (reader->len != 0 && reader->data == NULL)) {
        return -1;
    }
    if (reader->offset == reader->len) {
        return 0;
    }

    cursor = *reader;
    if (!pb_read_varint(&cursor, &tag)) {
        return -1;
    }
    number = tag >> 3;
    if (number == 0 || number > PB_MAX_FIELD_NUMBER) {
        return -1;
    }
    result.number = (uint32_t) number;
    result.wire = (uint8_t) (tag & UINT64_C(7));

    switch (result.wire) {
        case 0:
            if (!pb_read_varint(&cursor, &result.integer)) {
                return -1;
            }
            break;
        case 1:
            result.length = 8;
            break;
        case 2:
            if (!pb_read_varint(&cursor, &length)
                || length > (uint64_t) (cursor.len - cursor.offset)) {
                return -1;
            }
            result.length = (size_t) length;
            break;
        case 5:
            result.length = 4;
            break;
        default:
            return -1;
    }

    if (result.wire != 0) {
        if (result.length > cursor.len - cursor.offset) {
            return -1;
        }
        result.bytes = cursor.data + cursor.offset;
        cursor.offset += result.length;
    }

    *reader = cursor;
    *field = result;
    return 1;
}
