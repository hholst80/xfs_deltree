CC = cc
CFLAGS = -O2 -g -std=c99 -fno-strict-aliasing -Wall -Wextra -Werror -Wno-unused-parameter
XFS = vendor/xfsprogs
CPPFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Dtypeof=__typeof__ -isystem $(XFS)/include -isystem $(XFS)/libxfs -isystem $(XFS)
CPPFLAGS += -DHAVE_LIBURCU_ATOMIC64
LIBS = $(XFS)/libxlog/.libs/libxlog.a $(XFS)/libxfs/.libs/libxfs.a $(XFS)/libfrog/.libs/libfrog.a
WRAP = -Wl,--wrap=pread64,--wrap=pwrite64,--wrap=write,--wrap=crc32c_le,--wrap=xfs_imap

.PHONY: all libs clean test test-crc sanitize
all: xfs_deltree

$(XFS)/configure:
	$(MAKE) -C $(XFS) configure

$(XFS)/include/builddefs: $(XFS)/configure
	cd $(XFS) && ./configure --disable-gettext --disable-libicu

libs: $(XFS)/include/builddefs
	$(MAKE) -C $(XFS) headers
	$(MAKE) -C $(XFS) libxfs libxlog

xfs_deltree: xfs_deltree.o walker.o stage.o bulk_inode.o bulk_extent.o fast_crc.o progress.o inode_io.o libs
	$(CC) $(LDFLAGS) -o $@ xfs_deltree.o walker.o stage.o bulk_inode.o bulk_extent.o fast_crc.o progress.o inode_io.o $(WRAP) -Wl,--start-group $(LIBS) -Wl,--end-group -luuid -lurcu -lurcu-cds -lpthread -lblkid -lm -lsqlite3

xfs_deltree.o walker.o bulk_inode.o bulk_extent.o inode_io.o: | libs
xfs_deltree.o: xfs_deltree.c stage.h walker.h bulk_inode.h bulk_extent.h
bulk_inode.o: bulk_inode.c bulk_inode.h stage.h
bulk_extent.o: bulk_extent.c bulk_extent.h stage.h
walker.o: walker.c walker.h
stage.o: stage.c stage.h

test-crc: fast_crc.o libs
	$(CC) $(CFLAGS) -o tests/test_crc tests/test_crc.c fast_crc.o -Wl,--wrap=crc32c_le $(XFS)/libfrog/.libs/libfrog.a
	./tests/test_crc

test: xfs_deltree test-crc
	python3 tests/test_offline.py

sanitize: libs
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -o xfs_deltree_sanitize xfs_deltree.c walker.c stage.c bulk_inode.c bulk_extent.c fast_crc.c progress.c inode_io.c $(WRAP) -Wl,--start-group $(LIBS) -Wl,--end-group -luuid -lurcu -lurcu-cds -lpthread -lblkid -lm -lsqlite3
	ASAN_OPTIONS=detect_leaks=0 XFS_DELTREE_BINARY=$(CURDIR)/xfs_deltree_sanitize python3 tests/test_offline.py

clean:
	rm -f tests/test_crc xfs_deltree xfs_deltree_sanitize xfs_deltree.o walker.o stage.o bulk_inode.o bulk_extent.o fast_crc.o progress.o inode_io.o

xfs_deltree.o stage.o bulk_extent.o progress.o: progress.h

xfs_deltree.o stage.o walker.o bulk_extent.o: config.h

inode_io.o: inode_io.c
