CC = gcc
CFLAGS = -Wall -Wextra -O2

all: client serverSelect serverThreads

client: Client.c
	$(CC) $(CFLAGS) -o client Client.c

serverSelect: ServerSelect.c
	$(CC) $(CFLAGS) -o serverSelect ServerSelect.c

serverThreads: ServerThreads.c
	$(CC) $(CFLAGS) -o serverThreads ServerThreads.c

clean:
	rm -f client serverSelect serverThreads