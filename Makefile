CC ?= gcc
CFLAGS ?= -O2 -Wall

.PHONY: all clean

all: code

code: main.c buddy.c buddy.h utils.h
	$(CC) $(CFLAGS) -o code main.c buddy.c

test: main.c buddy.c buddy.h utils.h
	$(CC) $(CFLAGS) -o test main.c buddy.c

clean:
	rm -f code test *.o
