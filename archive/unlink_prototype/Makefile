CC = cc
CFLAGS = -O3 -std=c99 -Wall -Wextra -Wpedantic -Werror
PREFIX = /usr/local

.PHONY: all test clean install
all: xfs_deltree

xfs_deltree: xfs_deltree.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< -pthread $(LDLIBS)

test: xfs_deltree
	python3 tests/test_deltree.py ./xfs_deltree

install: xfs_deltree
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 xfs_deltree $(DESTDIR)$(PREFIX)/bin/xfs_deltree

clean:
	rm -f xfs_deltree
