# Makefile for calci, a command-line calculator.
#
#   make                build ./calci
#   make test           build, then run the unit, command-line and
#                       interactive tests (the last need python3)
#   make install        copy calci to $(PREFIX)/bin (default /usr/local/bin)
#   make uninstall      remove it again
#   make clean          remove everything the build created
#
# CFLAGS and LDFLAGS may be overridden, e.g. make CFLAGS="-O0 -g"; the
# language standard and the warnings are always applied.

CFLAGS ?= -O2
PREFIX ?= /usr/local
PYTHON ?= python3
WARN    = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
          -Wmissing-prototypes -Wformat=2 -Wundef
BUILD   = $(CC) -std=c99 $(WARN) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS)
LDLIBS  = -lm

SOURCES = src/main.c src/calc.c src/editor.c src/term.c
HEADERS = src/calc.h src/editor.h src/term.h

all: calci

calci: $(SOURCES) $(HEADERS)
	$(BUILD) -o $@ $(SOURCES) $(LDLIBS)

tests/test_calc: tests/test_calc.c src/calc.c src/calc.h
	$(BUILD) -Isrc -o $@ tests/test_calc.c src/calc.c $(LDLIBS)

test: calci tests/test_calc
	./tests/test_calc
	sh tests/cli.sh ./calci
	@if command -v $(PYTHON) >/dev/null 2>&1; then \
	    echo "$(PYTHON) tests/interactive.py ./calci"; \
	    $(PYTHON) tests/interactive.py ./calci; \
	else \
	    echo "skipping tests/interactive.py: $(PYTHON) not found"; \
	fi

install: calci
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp calci $(DESTDIR)$(PREFIX)/bin/calci
	chmod 755 $(DESTDIR)$(PREFIX)/bin/calci

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/calci

clean:
	rm -f calci calci.exe tests/test_calc tests/test_calc.exe

.PHONY: all test install uninstall clean
