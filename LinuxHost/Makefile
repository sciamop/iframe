# iframe-linux-host. Needs: libavcodec-dev libavutil-dev libx11-dev libxext-dev libxdamage-dev
# libxfixes-dev libxrandr-dev libxtst-dev, an NVIDIA GPU (NVENC), and avahi-utils for Bonjour.
PREFIX ?= $(HOME)/.local
PKGS = libavcodec libavutil x11 xext xdamage xfixes xrandr xtst
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm -lpthread

# Protocol helpers shared with the Linux client (framing, JSON, key table).
SHARED = ../Linux/src/net.c ../Linux/src/json.c ../Linux/src/keymap.c
SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o) $(SHARED:../Linux/src/%.c=build/shared-%.o)
HDR = src/host.h ../Linux/src/iframe.h ../Linux/src/net.h

build/iframe-linux-host: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(HDR) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/shared-%.o: ../Linux/src/%.c $(HDR) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

install: build/iframe-linux-host
	install -Dm755 build/iframe-linux-host $(PREFIX)/bin/iframe-linux-host

clean:
	rm -rf build

.PHONY: install clean
