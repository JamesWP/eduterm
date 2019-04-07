LDLIBS += -lX11
CFLAGS += -std=c11 -Wall -Wextra -g -O3

.PHONY: all clean

all: eduterm

eduterm: eduterm.c

clean:
	rm eduterm
