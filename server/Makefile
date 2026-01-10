.PHONY: all default

default: all

all: clean aesdsocket

aesdsocket: aesdsocket.c
	$(CC) -o aesdsocket aesdsocket.c

clean:
	rm -f *.o aesdsocket