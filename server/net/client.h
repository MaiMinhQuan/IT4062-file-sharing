#ifndef CLIENT_H
#define CLIENT_H

#include <sys/socket.h>

#define BUFFER_SIZE 24576
#define SEND_BUFFER_SIZE 32768
#define MAX_CLIENTS 30

typedef struct {
    int sock;
    char recv_buf[BUFFER_SIZE];
    int recv_len;

    char send_buf[SEND_BUFFER_SIZE];
    int send_len;
    int send_offset;

    int authenticated;
} Client;

extern Client clients[MAX_CLIENTS];

void init_clients();
int add_client(int sock);
void remove_client_index(int idx);

#endif
