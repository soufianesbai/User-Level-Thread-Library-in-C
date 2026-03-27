CC = gcc
CFLAGS = -Wall -Wextra -Werror

all: thread.o

thread.o: thread.c thread.h
	$(CC) $(CFLAGS) -c thread.c -o thread.o

clean:
	rm -f *.o
