# Makefile for lab 6, part 1

CC      = gcc -arch arm64
CFLAGS  = -g -Wall -I../../lab3/solutions/part1
LDFLAGS = -L../../lab3/solutions/part1
LDLIBS  = -lmylist

mdb-lookup-server: mdb-lookup-server.o

mdb-lookup-server.o: mdb.h

.PHONY: clean
clean:
	rm -f *.o a.out mdb-lookup-server

.PHONY: all
al: clean default
