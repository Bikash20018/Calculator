# Makefile for calci, a command-line calculator.
#
#   make                build ./calci
#   make test           build, then run the unit and command-line tests
#   make install        copy calci to $(PREFIX)/bin (default /usr/local/bin)
#   make uninstall      remove it again
#   make clean          remove everything the build created
#
# CFLAGS and LDFLAGS may be overridden, e.g. make CFLAGS="-O0 -g"; the
# language standard and the warnings are always applied.

CFLAGS ?= -O2
PREFIX ?= /usr/local
WARN    = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
          -Wmissing-prototypes -Wformat=2 -Wundef
BUILD   = $(CC) -std=c99 $(WARN) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS)
LDLIBS  = -lm

all: calci

calci: src/main.c src/calc.c src/calc.h
	$(BUILD) -o $@ src/main.c src/calc.c $(LDLIBS)

tests/test_calc: tests/test_calc.c src/calc.c src/calc.h
	$(BUILD) -Isrc -o $@ tests/test_calc.c src/calc.c $(LDLIBS)

test: calci tests/test_calc
	./tests/test_calc
	sh tests/cli.sh ./calci

install: calci
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp calci $(DESTDIR)$(PREFIX)/bin/calci
	chmod 755 $(DESTDIR)$(PREFIX)/bin/calci

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/calci

clean:
	rm -f calci calci.exe tests/test_calc tests/test_calc.exe

.PHONY: all test install uninstall clean
