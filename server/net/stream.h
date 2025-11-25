#ifndef STREAM_H
#define STREAM_H

int enqueue_send(int idx, const char *data, int len);
int flush_send(int idx);
int find_crlf(const char *buf, int len);

#endif
