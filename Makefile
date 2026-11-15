CC      ?= cc
PKG     := wlroots-0.20 wayland-server xkbcommon lua5.4 cairo
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -DWLR_USE_UNSTABLE -Wall -Wextra \
           -Wno-unused-parameter $(shell pkg-config --cflags $(PKG))
LDLIBS  += $(shell pkg-config --libs $(PKG))
PREFIX  ?= /usr/local

BIN := nide

all: $(BIN)

$(BIN): src/main.c
	$(CC) $(CFLAGS) src/main.c -o $@ $(LDLIBS)

check: $(BIN)
	./$(BIN) --check -c config/config.lua

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/nide
	install -Dm644 config/config.lua \
		$(DESTDIR)$(PREFIX)/share/nide/config.lua
	install -Dm644 nide.desktop \
		$(DESTDIR)$(PREFIX)/share/wayland-sessions/nide.desktop
	@echo "Copy config: mkdir -p ~/.config/nide && cp config/config.lua ~/.config/nide/"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nide
	rm -f $(DESTDIR)$(PREFIX)/share/nide/config.lua
	rm -f $(DESTDIR)$(PREFIX)/share/wayland-sessions/nide.desktop

clean:
	rm -f $(BIN)

.PHONY: all check install uninstall clean
