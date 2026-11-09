#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_riak_pb.h"
#include "Zend/zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"
#include "ext/standard/info.h"
#include "pb_codec.h"
#include "riak_transport.h"
#include <string.h>

enum {
    RIAK_ERROR = 0,
    RIAK_PING_REQ = 1, RIAK_PING_RESP = 2,
    RIAK_SERVER_INFO_REQ = 7, RIAK_SERVER_INFO_RESP = 8,
    RIAK_GET_REQ = 9, RIAK_GET_RESP = 10,
    RIAK_PUT_REQ = 11, RIAK_PUT_RESP = 12,
    RIAK_DELETE_REQ = 13, RIAK_DELETE_RESP = 14,
    RIAK_SET_BUCKET_REQ = 21, RIAK_SET_BUCKET_RESP = 22
};

static void protocol_error(void)
{
    zend_throw_exception(spl_ce_RuntimeException, "Malformed Riak PB response", 0);
}

static bool validate_endpoint(zend_string *host, zend_long port, zend_long timeout_ms)
{
    if (ZSTR_LEN(host) == 0 || ZSTR_LEN(host) > 253) {
        zend_value_error("host must be a hostname or IP address without a scheme or port");
        return false;
    }
    for (size_t i = 0; i < ZSTR_LEN(host); i++) {
        unsigned char c = (unsigned char) ZSTR_VAL(host)[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':')) {
            zend_value_error("host must be a hostname or unbracketed IP address");
            return false;
        }
    }
    if (port < 1 || port > 65535) {
        zend_value_error("port must be between 1 and 65535");
        return false;
    }
    if (timeout_ms < 1 || timeout_ms > 600000) {
        zend_value_error("timeout_ms must be between 1 and 600000");
        return false;
    }
    return true;
}

static bool validate_name(zend_string *name, const char *label)
{
    if (ZSTR_LEN(name) == 0 || ZSTR_LEN(name) >= RIAK_MAX_FRAME_SIZE) {
        zend_value_error("%s must be non-empty and smaller than the 64 MiB frame limit", label);
        return false;
    }
    return true;
}

static bool validate_mime(zend_string *mime)
{
    if (ZSTR_LEN(mime) == 0 || ZSTR_LEN(mime) > 255) {
        zend_value_error("content_type must be a MIME type");
        return false;
    }
    size_t slash_count = 0;
    for (size_t i = 0; i < ZSTR_LEN(mime); i++) {
        unsigned char c = (unsigned char) ZSTR_VAL(mime)[i];
        if (c == '/') {
            if (++slash_count > 1 || i == 0 || i == ZSTR_LEN(mime) - 1) {
                zend_value_error("content_type must be a MIME type");
                return false;
            }
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || (c != 0 && strchr("!#$&^_.+-", c)))) {
            zend_value_error("content_type must be a MIME type without parameters");
            return false;
        }
    }
    if (slash_count != 1) {
        zend_value_error("content_type must contain a type and subtype");
        return false;
    }
    return true;
}

static bool encode_string(pb_buffer *buffer, uint32_t field, zend_string *value)
{
    return pb_add_bytes(buffer, field, ZSTR_VAL(value), ZSTR_LEN(value));
}

static void encoding_error(void)
{
    zend_throw_exception(spl_ce_RuntimeException, "Cannot encode Riak PB request: size limit or allocation failure", 0);
}

static void server_error(const riak_reply *reply)
{
    pb_reader reader;
    pb_field field;
    const char *message = NULL;
    size_t length = 0;
    uint64_t code = 0;
    bool has_code = false;
    int status;
    pb_reader_init(&reader, reply->data, reply->length);
    while ((status = pb_next(&reader, &field)) == 1) {
        if (field.number == 1) {
            if (field.wire != 2) {
                protocol_error();
                return;
            }
            message = (const char *) field.bytes;
            length = field.length;
        } else if (field.number == 2) {
            if (field.wire != 0 || field.integer > UINT32_MAX) {
                protocol_error();
                return;
            }
            code = field.integer;
            has_code = true;
        }
    }
    if (status < 0 || !message || !has_code) {
        protocol_error();
        return;
    }
    zend_throw_exception_ex(spl_ce_RuntimeException, (zend_long) code,
        "Riak PB server error: %.*s", (int) (length > 1024 ? 1024 : length), message);
}

static bool request(zend_string *host, zend_long port, zend_long timeout_ms,
    uint8_t code, const pb_buffer *payload, uint8_t expected, riak_reply *reply)
{
    char error[512];
    const uint8_t *data = payload ? payload->data : NULL;
    size_t length = payload ? payload->len : 0;
    if (!riak_exchange(ZSTR_VAL(host), (unsigned int) port, (unsigned int) timeout_ms,
        code, data, length, reply, error, sizeof(error))) {
        zend_throw_exception(spl_ce_RuntimeException, error, 0);
        return false;
    }
    if (reply->code == RIAK_ERROR) {
        server_error(reply);
        riak_reply_free(reply);
        return false;
    }
    if (reply->code != expected) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "Unexpected Riak PB response code %u; expected %u", reply->code, expected);
        riak_reply_free(reply);
        return false;
    }
    return true;
}

static bool validate_message(const uint8_t *data, size_t length)
{
    pb_reader reader;
    pb_field field;
    pb_reader_init(&reader, data, length);
    int status;
    while ((status = pb_next(&reader, &field)) == 1) {
        /* Unknown fields are valid for forward-compatible protobuf messages. */
    }
    if (status < 0) {
        protocol_error();
        return false;
    }
    return true;
}

static bool command(zend_string *host, zend_long port, zend_long timeout_ms,
    uint8_t code, const pb_buffer *payload, uint8_t expected)
{
    riak_reply reply;
    if (!request(host, port, timeout_ms, code, payload, expected, &reply)) {
        return false;
    }
    bool success = validate_message(reply.data, reply.length);
    riak_reply_free(&reply);
    return success;
}

static bool decode_content(const uint8_t *data, size_t length, zval *result)
{
    pb_reader reader;
    pb_field field;
    const uint8_t *value = NULL;
    size_t value_length = 0;
    const char *mime = "application/octet-stream";
    size_t mime_length = sizeof("application/octet-stream") - 1;
    bool has_value = false, deleted = false;
    int status;
    pb_reader_init(&reader, data, length);
    while ((status = pb_next(&reader, &field)) == 1) {
        if (field.number == 1 || field.number == 2) {
            if (field.wire != 2) {
                protocol_error();
                return false;
            }
            if (field.number == 1) {
                value = field.bytes;
                value_length = field.length;
                has_value = true;
            } else if (field.length > 0) {
                mime = (const char *) field.bytes;
                mime_length = field.length;
            }
        } else if (field.number == 11) {
            if (field.wire != 0) {
                protocol_error();
                return false;
            }
            deleted = field.integer != 0;
        }
    }
    if (status < 0 || !has_value) {
        protocol_error();
        return false;
    }
    if (deleted) {
        ZVAL_NULL(result);
        return true;
    }
    array_init(result);
    add_assoc_stringl(result, "content", (const char *) value, value_length);
    add_assoc_stringl(result, "content_type", mime, mime_length);
    return true;
}

static bool decode_get(const riak_reply *reply, zval *result)
{
    pb_reader reader;
    pb_field field;
    const uint8_t *content = NULL;
    size_t content_length = 0, count = 0;
    int status;
    pb_reader_init(&reader, reply->data, reply->length);
    while ((status = pb_next(&reader, &field)) == 1) {
        if (field.number == 1) {
            if (field.wire != 2) {
                protocol_error();
                return false;
            }
            content = field.bytes;
            content_length = field.length;
            count++;
        }
    }
    if (status < 0) {
        protocol_error();
        return false;
    }
    if (count > 1) {
        zend_throw_exception(spl_ce_RuntimeException,
            "Riak PB returned multiple siblings; resolve the conflict before reading this file", 0);
        return false;
    }
    if (count == 0) {
        ZVAL_NULL(result);
        return true;
    }
    return decode_content(content, content_length, result);
}

PHP_FUNCTION(riak_pb_ping)
{
    zend_string *host;
    zend_long port = 8129, timeout_ms = 3000;
    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_STR(host)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(port)
        Z_PARAM_LONG(timeout_ms)
    ZEND_PARSE_PARAMETERS_END();
    if (!validate_endpoint(host, port, timeout_ms)
        || !command(host, port, timeout_ms, RIAK_PING_REQ, NULL, RIAK_PING_RESP)) {
        RETURN_THROWS();
    }
    RETURN_TRUE;
}

PHP_FUNCTION(riak_pb_server_info)
{
    zend_string *host;
    zend_long port = 8129, timeout_ms = 3000;
    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_STR(host)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(port)
        Z_PARAM_LONG(timeout_ms)
    ZEND_PARSE_PARAMETERS_END();
    if (!validate_endpoint(host, port, timeout_ms)) {
        RETURN_THROWS();
    }
    riak_reply reply;
    if (!request(host, port, timeout_ms, RIAK_SERVER_INFO_REQ, NULL, RIAK_SERVER_INFO_RESP, &reply)) {
        RETURN_THROWS();
    }
    const char *node = "", *version = "";
    size_t node_length = 0, version_length = 0;
    pb_reader reader;
    pb_field field;
    int status;
    pb_reader_init(&reader, reply.data, reply.length);
    while ((status = pb_next(&reader, &field)) == 1) {
        if (field.number == 1 || field.number == 2) {
            if (field.wire != 2) {
                status = -1;
                break;
            }
            if (field.number == 1) {
                node = (const char *) field.bytes;
                node_length = field.length;
            } else {
                version = (const char *) field.bytes;
                version_length = field.length;
            }
        }
    }
    if (status < 0) {
        riak_reply_free(&reply);
        protocol_error();
        RETURN_THROWS();
    }
    array_init(return_value);
    add_assoc_stringl(return_value, "node", node, node_length);
    add_assoc_stringl(return_value, "server_version", version, version_length);
    riak_reply_free(&reply);
}

PHP_FUNCTION(riak_pb_get)
{
    zend_string *host, *bucket, *key;
    zend_long port, timeout_ms = 3000;
    ZEND_PARSE_PARAMETERS_START(4, 5)
        Z_PARAM_STR(host)
        Z_PARAM_LONG(port)
        Z_PARAM_STR(bucket)
        Z_PARAM_STR(key)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(timeout_ms)
    ZEND_PARSE_PARAMETERS_END();
    if (!validate_endpoint(host, port, timeout_ms)
        || !validate_name(bucket, "bucket") || !validate_name(key, "key")) {
        RETURN_THROWS();
    }
    pb_buffer payload;
    pb_buffer_init(&payload);
    if (!encode_string(&payload, 1, bucket) || !encode_string(&payload, 2, key)
        || !pb_add_uint(&payload, 10, (uint64_t) timeout_ms)) {
        pb_buffer_free(&payload);
        encoding_error();
        RETURN_THROWS();
    }
    riak_reply reply;
    bool success = request(host, port, timeout_ms, RIAK_GET_REQ, &payload, RIAK_GET_RESP, &reply);
    pb_buffer_free(&payload);
    if (!success) {
        RETURN_THROWS();
    }
    success = decode_get(&reply, return_value);
    riak_reply_free(&reply);
    if (!success) {
        RETURN_THROWS();
    }
}

PHP_FUNCTION(riak_pb_put)
{
    zend_string *host, *bucket, *key, *content, *mime = NULL;
    zend_long port, timeout_ms = 3000;
    ZEND_PARSE_PARAMETERS_START(5, 7)
        Z_PARAM_STR(host)
        Z_PARAM_LONG(port)
        Z_PARAM_STR(bucket)
        Z_PARAM_STR(key)
        Z_PARAM_STR(content)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(mime)
        Z_PARAM_LONG(timeout_ms)
    ZEND_PARSE_PARAMETERS_END();
    if (!validate_endpoint(host, port, timeout_ms) || !validate_name(bucket, "bucket")
        || !validate_name(key, "key") || (mime && !validate_mime(mime))) {
        RETURN_THROWS();
    }
    if (ZSTR_LEN(content) >= RIAK_MAX_FRAME_SIZE) {
        zend_value_error("content exceeds the 64 MiB frame limit");
        RETURN_THROWS();
    }
    pb_buffer object, payload;
    pb_buffer_init(&object);
    pb_buffer_init(&payload);
    const char *mime_data = mime ? ZSTR_VAL(mime) : "application/octet-stream";
    size_t mime_length = mime ? ZSTR_LEN(mime) : sizeof("application/octet-stream") - 1;
    bool encoded = encode_string(&object, 1, content)
        && pb_add_bytes(&object, 2, mime_data, mime_length)
        && encode_string(&payload, 1, bucket) && encode_string(&payload, 2, key)
        && pb_add_bytes(&payload, 4, object.data, object.len)
        && pb_add_uint(&payload, 7, 0)
        && pb_add_uint(&payload, 12, (uint64_t) timeout_ms);
    pb_buffer_free(&object);
    if (!encoded) {
        pb_buffer_free(&payload);
        encoding_error();
        RETURN_THROWS();
    }
    bool success = command(host, port, timeout_ms, RIAK_PUT_REQ, &payload, RIAK_PUT_RESP);
    pb_buffer_free(&payload);
    if (!success) {
        RETURN_THROWS();
    }
    RETURN_TRUE;
}

PHP_FUNCTION(riak_pb_delete)
{
    zend_string *host, *bucket, *key;
    zend_long port, timeout_ms = 3000;
    ZEND_PARSE_PARAMETERS_START(4, 5)
        Z_PARAM_STR(host)
        Z_PARAM_LONG(port)
        Z_PARAM_STR(bucket)
        Z_PARAM_STR(key)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(timeout_ms)
    ZEND_PARSE_PARAMETERS_END();
    if (!validate_endpoint(host, port, timeout_ms)
        || !validate_name(bucket, "bucket") || !validate_name(key, "key")) {
        RETURN_THROWS();
    }
    pb_buffer payload;
    pb_buffer_init(&payload);
    if (!encode_string(&payload, 1, bucket) || !encode_string(&payload, 2, key)
        || !pb_add_uint(&payload, 10, (uint64_t) timeout_ms)) {
        pb_buffer_free(&payload);
        encoding_error();
        RETURN_THROWS();
    }
    bool success = command(host, port, timeout_ms, RIAK_DELETE_REQ, &payload, RIAK_DELETE_RESP);
    pb_buffer_free(&payload);
    if (!success) {
        RETURN_THROWS();
    }
    RETURN_TRUE;
}

PHP_FUNCTION(riak_pb_set_bucket_allow_mult)
{
    zend_string *host, *bucket;
    zend_long port, timeout_ms = 3000;
    bool allow_mult;
    ZEND_PARSE_PARAMETERS_START(4, 5)
        Z_PARAM_STR(host)
        Z_PARAM_LONG(port)
        Z_PARAM_STR(bucket)
        Z_PARAM_BOOL(allow_mult)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(timeout_ms)
    ZEND_PARSE_PARAMETERS_END();
    if (!validate_endpoint(host, port, timeout_ms) || !validate_name(bucket, "bucket")) {
        RETURN_THROWS();
    }
    pb_buffer properties, payload;
    pb_buffer_init(&properties);
    pb_buffer_init(&payload);
    bool encoded = pb_add_uint(&properties, 2, allow_mult ? 1 : 0)
        && encode_string(&payload, 1, bucket)
        && pb_add_bytes(&payload, 2, properties.data, properties.len);
    pb_buffer_free(&properties);
    if (!encoded) {
        pb_buffer_free(&payload);
        encoding_error();
        RETURN_THROWS();
    }
    bool success = command(host, port, timeout_ms, RIAK_SET_BUCKET_REQ, &payload, RIAK_SET_BUCKET_RESP);
    pb_buffer_free(&payload);
    if (!success) {
        RETURN_THROWS();
    }
    RETURN_TRUE;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_ping, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, port, IS_LONG, 0, "8129")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_ms, IS_LONG, 0, "3000")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_server_info, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, port, IS_LONG, 0, "8129")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_ms, IS_LONG, 0, "3000")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_get, 0, 4, IS_ARRAY, 1)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, bucket, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_ms, IS_LONG, 0, "3000")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_delete, 0, 4, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, bucket, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_ms, IS_LONG, 0, "3000")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_put, 0, 5, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, bucket, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, content, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, content_type, IS_STRING, 0, "\"application/octet-stream\"")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_ms, IS_LONG, 0, "3000")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_set_bucket, 0, 4, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, bucket, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, allow_mult, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_ms, IS_LONG, 0, "3000")
ZEND_END_ARG_INFO()

static const zend_function_entry riak_pb_functions[] = {
    PHP_FE(riak_pb_ping, arginfo_ping)
    PHP_FE(riak_pb_server_info, arginfo_server_info)
    PHP_FE(riak_pb_get, arginfo_get)
    PHP_FE(riak_pb_put, arginfo_put)
    PHP_FE(riak_pb_delete, arginfo_delete)
    PHP_FE(riak_pb_set_bucket_allow_mult, arginfo_set_bucket)
    PHP_FE_END
};

PHP_MINFO_FUNCTION(riak_pb)
{
    (void) zend_module;
    php_info_print_table_start();
    php_info_print_table_row(2, "Riak PB file client", "enabled");
    php_info_print_table_row(2, "Version", PHP_RIAK_PB_VERSION);
    php_info_print_table_row(2, "Transport", "plain TCP; no TLS or authentication");
    php_info_print_table_row(2, "Maximum frame", "64 MiB");
    php_info_print_table_end();
}

zend_module_entry riak_pb_module_entry = {
    STANDARD_MODULE_HEADER,
    "riak_pb",
    riak_pb_functions,
    NULL, NULL, NULL, NULL,
    PHP_MINFO(riak_pb),
    PHP_RIAK_PB_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_RIAK_PB
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(riak_pb)
#endif
