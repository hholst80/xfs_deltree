CC = cc
CFLAGS = -O2 -g -std=c99 -fno-strict-aliasing -Wall -Wextra -Werror -Wno-unused-parameter
XFS = vendor/xfsprogs
CPPFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Dtypeof=__typeof__ -isystem $(XFS)/include -isystem $(XFS)/libxfs -isystem $(XFS)
CPPFLAGS += -DHAVE_LIBURCU_ATOMIC64
LIBS = $(XFS)/libxlog/.libs/libxlog.a $(XFS)/libxfs/.libs/libxfs.a $(XFS)/libfrog/.libs/libfrog.a
WRAP = -Wl,--wrap=pread64,--wrap=pwrite64,--wrap=write

.PHONY: all libs clean test sanitize
all: xfs_deltree

$(XFS)/configure:
	$(MAKE) -C $(XFS) configure

$(XFS)/include/builddefs: $(XFS)/configure
	cd $(XFS) && ./configure --disable-gettext --disable-libicu

libs: $(XFS)/include/builddefs
	$(MAKE) -C $(XFS) headers
	$(MAKE) -C $(XFS) libxfs libxlog

xfs_deltree: xfs_deltree.o walker.o stage.o libs
	$(CC) $(LDFLAGS) -o $@ xfs_deltree.o walker.o stage.o $(WRAP) -Wl,--start-group $(LIBS) -Wl,--end-group -luuid -lurcu -lurcu-cds -lpthread -lblkid -lm

xfs_deltree.o walker.o: | libs
xfs_deltree.o: xfs_deltree.c stage.h walker.h
walker.o: walker.c walker.h
stage.o: stage.c stage.h

test: xfs_deltree
	python3 tests/test_offline.py

sanitize: libs
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -o xfs_deltree_sanitize xfs_deltree.c walker.c stage.c $(WRAP) -Wl,--start-group $(LIBS) -Wl,--end-group -luuid -lurcu -lurcu-cds -lpthread -lblkid -lm
	ASAN_OPTIONS=detect_leaks=0 XFS_DELTREE_BINARY=$(CURDIR)/xfs_deltree_sanitize python3 tests/test_offline.py

clean:
	rm -f xfs_deltree xfs_deltree_sanitize xfs_deltree.o walker.o stage.o
