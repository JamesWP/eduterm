LDLIBS += -lX11
CFLAGS += -std=c11 -Wall -Wextra -O3

DEBUG=yes

ifdef PROF
  CFLAGS += -g -pg
  LDFLAGS += -pg
else
  ifdef DEBUG
    CFLAGS += -g
  endif
endif

.PHONY: all clean docker-run docker

all: eduterm

eduterm: eduterm.c

clean:
	rm eduterm

docker:
	docker build . -t eduterm

docker-run: docker
	docker run -i -t --rm eduterm bash

