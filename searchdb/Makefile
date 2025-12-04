# Makefile for lab 6, part 1

CC      = gcc -arch arm64
CFLAGS  = -g -Wall
LDFLAGS = 
LDLIBS  = 

mdb-lookup-server: mdb-lookup-server.o mylist.o
	$(CC) $(CFLAGS) mdb-lookup-server.o mylist.o -o mdb-lookup-server

mdb-lookup-server.o: mdb.h mylist.h
	$(CC) $(CFLAGS) -c mdb-lookup-server.c

mylist.o: mylist.h
	$(CC) $(CFLAGS) -c mylist.c

.PHONY: clean
clean:
	rm -f *.o a.out mdb-lookup-server

.PHONY: all
all: clean default
