CC = gcc
CFLAGS = -Wall -g
LDFLAGS = -L


http-server : http-server.o
	$(CC) http-server.o -o http-server
http-server.o : http-server.c
	$(CC) $(CFLAGS) -c http-server.c
clean :
	rm -f *.o http-server
