#include "client.h"
#include <string.h>
#include <unistd.h>

Client clients[MAX_CLIENTS];

void init_clients() {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].sock = 0;
        clients[i].recv_len = 0;
        clients[i].send_len = 0;
        clients[i].send_offset = 0;
        clients[i].authenticated = 0;
        clients[i].user_id = 0;
    }
}

int add_client(int sock) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].sock == 0) {
            clients[i].sock = sock;
            clients[i].recv_len = 0;
            clients[i].send_len = 0;
            clients[i].send_offset = 0;
            clients[i].authenticated = 0;
            clients[i].user_id = 0;
            return i;
        }
    }
    return -1;
}

void remove_client_index(int idx) {
    if (clients[idx].sock != 0) {
        close(clients[idx].sock);
    }
    clients[idx].sock = 0;
    clients[idx].recv_len = 0;
    clients[idx].send_len = 0;
    clients[idx].send_offset = 0;
    clients[idx].authenticated = 0;
    clients[idx].user_id = 0;
}
