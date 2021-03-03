PKG_NAME = ddb_jack

PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib

CC     ?= gcc
CFLAGS += -I$(shell pkg-config --variable=includedir jack)
CFLAGS_ADD ?= -O2
LDLIBS += $(shell pkg-config --cflags --libs jack)

all:
	$(CC) -std=c99 -shared $(CFLAGS) $(CFLAGS_ADD) -o $(PKG_NAME).so $(PKG_NAME).c -fPIC -Wall $(LDLIBS) $(LDFLAGS)

install:
	install -D -m 644 $(PKG_NAME).so $(DESTDIR)$(LIBDIR)/deadbeef/$(PKG_NAME).so

clean:
	rm -f $(PKG_NAME).so

debug: CFLAGS_ADD = -g3 -ggdb -DDEBUG
debug: PREFIX = $(HOME)/.local/
debug: all install