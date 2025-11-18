CC = gcc
CFLAGS = -Wall -I/usr/include/openssl -I./database -I./user -I./group -I./file -I./token -g
LIBS = -lmysqlclient -lcrypto -lssl

SERVER_SRCS = server.c database/db.c
SERVER_OBJS = $(SERVER_SRCS:.c=.o)

CLIENT_SRCS = client.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

all: server client

server: $(SERVER_OBJS)
	$(CC) $(SERVER_OBJS) -o server $(LIBS)

client: $(CLIENT_OBJS)
	$(CC) $(CLIENT_OBJS) -o client

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS) server client

.PHONY: clean all
