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
    
    // Limit bytes per iteration to prevent blocking other clients
    int max_bytes_per_call = 32 * 1024;  // 32KB max per iteration
    int bytes_sent_this_call = 0;

    while (c->send_offset < c->send_len && bytes_sent_this_call < max_bytes_per_call) {
        int remaining = c->send_len - c->send_offset;
        int to_send = remaining;
        
        // Limit chunk size to not exceed max_bytes_per_call
        if (to_send > max_bytes_per_call - bytes_sent_this_call) {
            to_send = max_bytes_per_call - bytes_sent_this_call;
        }
        
        ssize_t n = send(c->sock,
                         c->send_buf + c->send_offset,
                         to_send,
                         0);

        if (n > 0) {
            c->send_offset += n;
            bytes_sent_this_call += n;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;  // Socket buffer full, will continue next iteration
        } else {
            return -1;
        }
    }

    // If we haven't sent everything, return 0 to continue in next select() iteration
    if (c->send_offset < c->send_len) {
        return 0;
    }

    // All data sent, reset buffer
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
