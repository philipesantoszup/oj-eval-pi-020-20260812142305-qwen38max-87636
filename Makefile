.PHONY: all clean

all: code

code: main.c buddy.c buddy.h utils.h
	gcc -O2 -Wall -Wno-int-conversion -Wno-implicit-function-declaration -Wno-error=int-conversion -Wno-error=implicit-function-declaration -o code main.c buddy.c

test: main.c buddy.c buddy.h utils.h
	gcc -O2 -Wall -Wno-int-conversion -Wno-implicit-function-declaration -o test main.c buddy.c

clean:
	rm -f code test *.o
