LDLIBS += -lX11
CFLAGS += -std=c99 -Wall -Wextra -g 

.PHONY: all clean

all: eduterm

eduterm: eduterm.c

clean:
	rm eduterm
