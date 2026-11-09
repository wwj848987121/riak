#include "../pb_codec.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void assert_invalid(const uint8_t *data, size_t length)
{
    pb_reader reader;
    pb_field field = {123, 0, 456, NULL, 0};

    pb_reader_init(&reader, data, length);
    assert(pb_next(&reader, &field) == -1);
    assert(reader.offset == 0);
    assert(field.number == 123 && field.integer == 456);
}

static void test_round_trip(void)
{
    static const uint8_t payload[] = {'a', 0, 'b', 0xff};
    static const uint8_t expected[] = {
        0x08, 0x96, 0x01, 0x12, 0x04, 'a', 0, 'b', 0xff, 0x1a, 0x00
    };
    pb_buffer buffer;
    pb_reader reader;
    pb_field field;

    pb_buffer_init(&buffer);
    assert(pb_add_uint(&buffer, 1, 150));
    assert(pb_add_bytes(&buffer, 2, payload, sizeof(payload)));
    assert(pb_add_bytes(&buffer, 3, NULL, 0));
    assert(buffer.len == sizeof(expected));
    assert(memcmp(buffer.data, expected, sizeof(expected)) == 0);

    pb_reader_init(&reader, buffer.data, buffer.len);
    assert(pb_next(&reader, &field) == 1);
    assert(field.number == 1 && field.wire == 0 && field.integer == 150);
    assert(field.bytes == NULL && field.length == 0);
    assert(pb_next(&reader, &field) == 1);
    assert(field.number == 2 && field.wire == 2 && field.length == sizeof(payload));
    assert(field.bytes == buffer.data + 5);
    assert(memcmp(field.bytes, payload, sizeof(payload)) == 0);
    assert(pb_next(&reader, &field) == 1);
    assert(field.number == 3 && field.wire == 2 && field.length == 0);
    assert(pb_next(&reader, &field) == 0);
    pb_buffer_free(&buffer);
    assert(buffer.data == NULL && buffer.len == 0 && buffer.capacity == 0);
    pb_buffer_free(&buffer);
}

static void test_integer_boundaries(void)
{
    static const uint64_t values[] = {
        0, 1, 127, 128, 16383, 16384, UINT64_C(0xffffffff),
        UINT64_C(0x8000000000000000), UINT64_MAX
    };
    pb_buffer buffer;
    pb_reader reader;
    pb_field field;
    size_t index;

    pb_buffer_init(&buffer);
    for (index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
        assert(pb_add_uint(&buffer, PB_MAX_FIELD_NUMBER, values[index]));
    }
    pb_reader_init(&reader, buffer.data, buffer.len);
    for (index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
        assert(pb_next(&reader, &field) == 1);
        assert(field.number == PB_MAX_FIELD_NUMBER);
        assert(field.integer == values[index]);
    }
    assert(pb_next(&reader, &field) == 0);
    pb_buffer_free(&buffer);
}

static void test_unknown_fixed_fields(void)
{
    static const uint8_t message[] = {
        0xa1, 0x06, 1, 2, 3, 4, 5, 6, 7, 8,
        0xad, 0x06, 9, 10, 11, 12,
        0x08, 0x01
    };
    pb_reader reader;
    pb_field field;

    pb_reader_init(&reader, message, sizeof(message));
    assert(pb_next(&reader, &field) == 1);
    assert(field.number == 100 && field.wire == 1 && field.length == 8);
    assert(field.bytes[0] == 1 && field.bytes[7] == 8);
    assert(pb_next(&reader, &field) == 1);
    assert(field.number == 101 && field.wire == 5 && field.length == 4);
    assert(field.bytes[0] == 9 && field.bytes[3] == 12);
    assert(pb_next(&reader, &field) == 1);
    assert(field.number == 1 && field.integer == 1);
    assert(pb_next(&reader, &field) == 0);
}

static void test_malformed_messages(void)
{
    static const uint8_t truncated_tag[] = {0x80};
    static const uint8_t truncated_uint[] = {0x08, 0x80};
    static const uint8_t missing_uint[] = {0x08};
    static const uint8_t truncated_length[] = {0x0a, 0x80};
    static const uint8_t truncated_bytes[] = {0x0a, 0x02, 'x'};
    static const uint8_t truncated_fixed64[] = {0x09, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t truncated_fixed32[] = {0x0d, 0, 0, 0};
    static const uint8_t field_zero[] = {0x00, 0x01};
    static const uint8_t oversized_field[] = {0x80, 0x80, 0x80, 0x80, 0x10, 0x00};
    static const uint8_t start_group[] = {0x0b};
    static const uint8_t end_group[] = {0x0c};
    static const uint8_t invalid_wire6[] = {0x0e};
    static const uint8_t invalid_wire7[] = {0x0f};
    static const uint8_t overflowing_uint[] = {
        0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02
    };
    static const uint8_t continuing_uint[] = {
        0x08, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00
    };
    static const uint8_t overflowing_tag[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02
    };
    static const uint8_t huge_length[] = {
        0x0a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01
    };

    assert_invalid(truncated_tag, sizeof(truncated_tag));
    assert_invalid(truncated_uint, sizeof(truncated_uint));
    assert_invalid(missing_uint, sizeof(missing_uint));
    assert_invalid(truncated_length, sizeof(truncated_length));
    assert_invalid(truncated_bytes, sizeof(truncated_bytes));
    assert_invalid(truncated_fixed64, sizeof(truncated_fixed64));
    assert_invalid(truncated_fixed32, sizeof(truncated_fixed32));
    assert_invalid(field_zero, sizeof(field_zero));
    assert_invalid(oversized_field, sizeof(oversized_field));
    assert_invalid(start_group, sizeof(start_group));
    assert_invalid(end_group, sizeof(end_group));
    assert_invalid(invalid_wire6, sizeof(invalid_wire6));
    assert_invalid(invalid_wire7, sizeof(invalid_wire7));
    assert_invalid(overflowing_uint, sizeof(overflowing_uint));
    assert_invalid(continuing_uint, sizeof(continuing_uint));
    assert_invalid(overflowing_tag, sizeof(overflowing_tag));
    assert_invalid(huge_length, sizeof(huge_length));
}

static void test_invalid_arguments(void)
{
    static const uint8_t byte = 0x08;
    pb_buffer buffer;
    pb_reader reader;
    pb_field field;
    size_t saved_length;

    pb_buffer_init(&buffer);
    assert(pb_add_uint(&buffer, 1, 1));
    saved_length = buffer.len;
    assert(!pb_add_uint(&buffer, 0, 1));
    assert(!pb_add_uint(&buffer, PB_MAX_FIELD_NUMBER + 1, 1));
    assert(!pb_add_bytes(&buffer, 0, &byte, 1));
    assert(!pb_add_bytes(&buffer, PB_MAX_FIELD_NUMBER + 1, &byte, 1));
    assert(!pb_add_bytes(&buffer, 1, NULL, 1));
    assert(!pb_add_bytes(&buffer, 1, &byte, PB_MAX_BUFFER_SIZE));
    assert(!pb_add_bytes(&buffer, 1, &byte, SIZE_MAX));
    assert(!pb_add_uint(NULL, 1, 1));
    assert(!pb_add_bytes(NULL, 1, &byte, 1));
    assert(buffer.len == saved_length && buffer.data[0] == 0x08 && buffer.data[1] == 0x01);
    pb_buffer_free(&buffer);

    pb_reader_init(&reader, NULL, 0);
    assert(pb_next(&reader, &field) == 0);
    pb_reader_init(&reader, NULL, 1);
    assert(pb_next(&reader, &field) == -1);
    pb_reader_init(&reader, &byte, PB_MAX_BUFFER_SIZE + 1);
    assert(pb_next(&reader, &field) == -1);
    pb_reader_init(&reader, &byte, 1);
    reader.offset = 2;
    assert(pb_next(&reader, &field) == -1);
    assert(pb_next(NULL, &field) == -1);
    assert(pb_next(&reader, NULL) == -1);
    pb_reader_init(NULL, NULL, 0);
    pb_buffer_init(NULL);
    pb_buffer_free(NULL);
}

static void test_aliased_append(void)
{
    uint8_t payload[60];
    pb_buffer buffer;
    pb_reader reader;
    pb_field field;

    memset(payload, 0xa5, sizeof(payload));
    pb_buffer_init(&buffer);
    assert(pb_add_bytes(&buffer, 1, payload, sizeof(payload)));
    assert(buffer.len == 62 && buffer.capacity == 64);
    assert(pb_add_bytes(&buffer, 2, buffer.data + 2, sizeof(payload)));
    assert(buffer.len == 124 && buffer.capacity >= 124);
    pb_reader_init(&reader, buffer.data, buffer.len);
    assert(pb_next(&reader, &field) == 1);
    assert(memcmp(field.bytes, payload, sizeof(payload)) == 0);
    assert(pb_next(&reader, &field) == 1);
    assert(field.number == 2 && field.length == sizeof(payload));
    assert(memcmp(field.bytes, payload, sizeof(payload)) == 0);
    assert(pb_next(&reader, &field) == 0);
    pb_buffer_free(&buffer);
}

static void test_buffer_limit(void)
{
    /* Fill exactly the limit using a length-delimited field and its header. */
    size_t payload_length = PB_MAX_BUFFER_SIZE - 5;
    uint8_t *payload = malloc(payload_length);
    pb_buffer buffer;
    pb_reader reader;
    pb_field field;

    assert(payload != NULL);
    memset(payload, 0x5a, payload_length);
    pb_buffer_init(&buffer);
    assert(pb_add_bytes(&buffer, 1, payload, payload_length));
    assert(buffer.len == PB_MAX_BUFFER_SIZE);
    assert(buffer.capacity == PB_MAX_BUFFER_SIZE);
    assert(!pb_add_uint(&buffer, 1, 0));
    assert(!pb_add_bytes(&buffer, 1, NULL, 0));
    assert(buffer.len == PB_MAX_BUFFER_SIZE);
    pb_reader_init(&reader, buffer.data, buffer.len);
    assert(pb_next(&reader, &field) == 1);
    assert(field.length == payload_length);
    assert(field.bytes[0] == 0x5a && field.bytes[payload_length - 1] == 0x5a);
    assert(pb_next(&reader, &field) == 0);
    pb_buffer_free(&buffer);
    free(payload);
}

int main(void)
{
    test_round_trip();
    test_integer_boundaries();
    test_unknown_fixed_fields();
    test_malformed_messages();
    test_invalid_arguments();
    test_aliased_append();
    test_buffer_limit();
    puts("codec tests passed");
    return 0;
}
