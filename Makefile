CC=gcc
CFLAGS = -g -Wall -Werror -lpthread

http-server: server.o
	$(CC) $(CFLAGS) http-server.o -o http-server

http-server.o: server.c
	$(CC) -c $(CFLAGS) http-server.c

.PHONY: all clean
all:
	make clean
	make

clean: 
	rm -f *.o http-server
