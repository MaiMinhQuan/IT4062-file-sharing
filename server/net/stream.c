#include "stream.h"
#include "client.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

int enqueue_send(int idx, const char *data, int len) {
    if (len <= 0) return 0;
    Client *c = &clients[idx];

    if (c->send_len + len > SEND_BUFFER_SIZE) return -1;

    memcpy(c->send_buf + c->send_len, data, len);
    c->send_len += len;

    return 0;
}

int flush_send(int idx) {
    Client *c = &clients[idx];

    while (c->send_offset < c->send_len) {
        ssize_t n = send(c->sock,
                         c->send_buf + c->send_offset,
                         c->send_len - c->send_offset,
                         0);

        if (n > 0) {
            c->send_offset += n;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            return -1;
        }
    }

    c->send_len = 0;
    c->send_offset = 0;
    return 0;
}

int find_crlf(const char *buf, int len) {
    for (int i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n') return i;
    }
    return -1;
}
