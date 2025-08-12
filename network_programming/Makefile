CC = gcc
CFLAGS = -Wall -g
LDFLAGS = -L


http-server : http-server.o
	$(CC) http-server.o -o http-server
http-server.o : 
	$(CC) $(CFLAGS) -c http-server.c
clean :
	rm *.o http-server
