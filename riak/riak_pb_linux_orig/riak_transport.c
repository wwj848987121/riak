#define _POSIX_C_SOURCE 200809L

#include "riak_transport.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool wait_socket(int fd, short events, int64_t deadline,
    char *error, size_t error_size)
{
    for (;;) {
        int64_t now = monotonic_ms();
        if (now < 0 || now >= deadline) {
            snprintf(error, error_size, "Riak PB operation timed out");
            return false;
        }
        int64_t remaining = deadline - now;
        struct pollfd descriptor = {fd, events, 0};
        int result = poll(&descriptor, 1, remaining > INT_MAX ? INT_MAX : (int) remaining);
        if (result > 0) {
            if (descriptor.revents & POLLNVAL) {
                snprintf(error, error_size, "Riak PB socket is invalid");
                return false;
            }
            /* Let send/recv/getsockopt report EOF, reset, and connect errors. */
            return true;
        }
        if (result == 0) {
            snprintf(error, error_size, "Riak PB operation timed out");
            return false;
        }
        if (errno != EINTR) {
            snprintf(error, error_size, "Riak PB poll failed: %s", strerror(errno));
            return false;
        }
    }
}

static int connect_socket(const char *host, unsigned int port, int64_t deadline,
    char *error, size_t error_size)
{
    struct addrinfo hints = {0};
    struct addrinfo *addresses = NULL;
    char service[6];
    int connected_fd = -1;
    int last_error = ECONNREFUSED;

    snprintf(service, sizeof(service), "%u", port);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    int result = getaddrinfo(host, service, &hints, &addresses);
    if (result != 0) {
        snprintf(error, error_size, "Riak PB address lookup failed: %s", gai_strerror(result));
        return -1;
    }

    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        int64_t now = monotonic_ms();
        if (now < 0 || now >= deadline) {
            last_error = ETIMEDOUT;
            break;
        }
        int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            last_error = errno;
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
            || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            last_error = errno;
            close(fd);
            continue;
        }
        result = connect(fd, address->ai_addr, address->ai_addrlen);
        if (result < 0 && errno == EINPROGRESS) {
            if (!wait_socket(fd, POLLOUT, deadline, error, error_size)) {
                close(fd);
                freeaddrinfo(addresses);
                return -1;
            }
            socklen_t size = sizeof(last_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &last_error, &size) < 0) {
                last_error = errno;
            }
            result = last_error == 0 ? 0 : -1;
        } else if (result < 0) {
            last_error = errno;
        }
        if (result == 0) {
            connected_fd = fd;
            break;
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    if (connected_fd < 0) {
        snprintf(error, error_size, "Riak PB connect to %s:%u failed: %s",
            host, port, strerror(last_error));
    }
    return connected_fd;
}

static bool transfer_bytes(int fd, void *buffer, size_t length, bool writing,
    int64_t deadline, char *error, size_t error_size)
{
    size_t offset = 0;
    while (offset < length) {
        if (!wait_socket(fd, writing ? POLLOUT : POLLIN, deadline, error, error_size)) {
            return false;
        }
        ssize_t count = writing
            ? send(fd, (const char *) buffer + offset, length - offset, MSG_NOSIGNAL)
            : recv(fd, (char *) buffer + offset, length - offset, 0);
        if (count > 0) {
            offset += (size_t) count;
        } else if (count == 0) {
            snprintf(error, error_size, "Riak PB connection closed before the frame was complete");
            return false;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            snprintf(error, error_size, "Riak PB %s failed: %s",
                writing ? "write" : "read", strerror(errno));
            return false;
        }
    }
    return true;
}

void riak_reply_free(riak_reply *reply)
{
    free(reply->data);
    reply->data = NULL;
    reply->length = 0;
    reply->code = 0;
}

bool riak_exchange(const char *host, unsigned int port, unsigned int timeout_ms,
    uint8_t code, const uint8_t *payload, size_t length, riak_reply *reply,
    char *error, size_t error_size)
{
    *reply = (riak_reply) {0};
    if (!host || !*host || port == 0 || port > 65535 || timeout_ms == 0
        || timeout_ms > 600000 || (length > 0 && !payload)) {
        snprintf(error, error_size, "Invalid Riak PB transport parameters");
        return false;
    }
    if (length >= RIAK_MAX_FRAME_SIZE) {
        snprintf(error, error_size, "Riak PB request exceeds the 64 MiB frame limit");
        return false;
    }
    int64_t start = monotonic_ms();
    if (start < 0) {
        snprintf(error, error_size, "Riak PB monotonic clock is unavailable");
        return false;
    }
    int64_t deadline = start + timeout_ms;
    int fd = connect_socket(host, port, deadline, error, error_size);
    if (fd < 0) {
        return false;
    }
    uint32_t size = (uint32_t) length + 1;
    uint8_t header[5] = {
        (uint8_t) (size >> 24), (uint8_t) (size >> 16),
        (uint8_t) (size >> 8), (uint8_t) size, code
    };
    bool success = false;
    if (!transfer_bytes(fd, header, sizeof(header), true, deadline, error, error_size)
        || !transfer_bytes(fd, (void *) payload, length, true, deadline, error, error_size)
        || !transfer_bytes(fd, header, 4, false, deadline, error, error_size)) {
        goto cleanup;
    }
    size = ((uint32_t) header[0] << 24) | ((uint32_t) header[1] << 16)
        | ((uint32_t) header[2] << 8) | header[3];
    if (size == 0 || size > RIAK_MAX_FRAME_SIZE) {
        snprintf(error, error_size, "Riak PB response has an invalid frame length (limit 64 MiB)");
        goto cleanup;
    }
    if (!transfer_bytes(fd, &reply->code, 1, false, deadline, error, error_size)) {
        goto cleanup;
    }
    reply->length = size - 1;
    if (reply->length > 0) {
        reply->data = malloc(reply->length);
        if (!reply->data) {
            snprintf(error, error_size, "Cannot allocate Riak PB response buffer");
            goto cleanup;
        }
        if (!transfer_bytes(fd, reply->data, reply->length, false, deadline, error, error_size)) {
            goto cleanup;
        }
    }
    success = true;
cleanup:
    close(fd);
    if (!success) {
        riak_reply_free(reply);
    }
    return success;
}
