# iFrame Linux client. Needs: libsdl2-dev libavcodec-dev libavutil-dev (and avahi-utils for discovery).
PREFIX ?= $(HOME)/.local
PKGS = sdl2 libavcodec libavutil
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm

SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)

build/iframe-client: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c src/iframe.h src/net.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

install: build/iframe-client
	install -Dm755 build/iframe-client $(PREFIX)/bin/iframe-client

clean:
	rm -rf build

.PHONY: install clean

# Protocol stand-in for testing without a Mac (see tools/fake-host.c).
build/fake-host: tools/fake-host.c build/net.o build/json.o | build
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -lpthread
