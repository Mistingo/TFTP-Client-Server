CC = gcc
CFLAGS = -Wall -Wextra -O2

all: server client

server: ServerBigfile.c
	$(CC) $(CFLAGS) -o server ServerBigfile.c

client: ClientBigfile.c
	$(CC) $(CFLAGS) -o client ClientBigfile.c

clean:
	rm -f server client
