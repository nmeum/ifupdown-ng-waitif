PREFIX  ?= /usr/local
EXECDIR ?= $(PREFIX)/libexec

CFLAGS ?= -O0 -g -Werror
CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809L
CFLAGS += -Wpedantic -Wall -Wextra \
	      -Wmissing-prototypes -Wpointer-arith \
	      -Wstrict-prototypes -Wshadow -Wformat-nonliteral

LDLIBS = -lmnl

waitif: waitif.c
install: waitif
	install -Dm755 waitif "$(DESTDIR)$(EXECDIR)/waitif"

.PHONY: install
