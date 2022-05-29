PREFIX  ?= /usr/local
EXECDIR ?= $(PREFIX)/libexec/ifupdown-ng
DOCDIR  ?= $(PREFIX)/share/doc/waitif
RUNDIR  ?= /var/run

CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CPPFLAGS += -DRUNDIR=\"$(RUNDIR)\"

CFLAGS ?= -O0 -g -Werror
CFLAGS += -std=c99
CFLAGS += -Wpedantic -Wall -Wextra \
	      -Wmissing-prototypes -Wpointer-arith \
	      -Wstrict-prototypes -Wshadow -Wformat-nonliteral

LDLIBS = -lmnl

waitif: waitif.c
install: waitif README.md
	install -Dm755 waitif "$(DESTDIR)$(EXECDIR)/waitif"
	install -Dm644 README.md "$(DESTDIR)$(DOCDIR)/README.md"

.PHONY: install
