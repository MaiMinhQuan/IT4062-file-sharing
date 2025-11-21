CC = gcc
CFLAGS = -Wall -g
LIBS = -lmysqlclient -lssl -lcrypto

SERVER_SRCS = main.c \
              database/db.c \
              auth/auth.c \
              auth/hash.c \
              auth/token.c \
              io/io_multiplexing.c \
              net/client.c \
              net/stream.c \
              protocol/command.c

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
	rm -f auth/*.o database/*.o io/*.o net/*.o protocol/*.o

.PHONY: clean all
