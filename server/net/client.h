#ifndef CLIENT_H
#define CLIENT_H

#include <sys/socket.h>

#define BUFFER_SIZE 24576
#define SEND_BUFFER_SIZE 65536  // 64KB - enough for 2 download chunks
#define MAX_CLIENTS 30

typedef struct {
    int sock;
    char recv_buf[BUFFER_SIZE];
    int recv_len;

    char send_buf[SEND_BUFFER_SIZE];
    int send_len;
    int send_offset;

    int authenticated;
    int user_id;  // ID của user sau khi đăng nhập
} Client;

extern Client clients[MAX_CLIENTS];

void init_clients();
int add_client(int sock);
void remove_client_index(int idx);

#endif
