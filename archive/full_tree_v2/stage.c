/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _FILE_OFFSET_BITS 64
#include "stage.h"
#include "progress.h"
#include "libfrog/crc32c.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

/* Linker interposition contains libxfs writes in a sector overlay. No source
 * write is allowed until the entire undo journal has reached stable storage.
 * The single-threaded command coalesces final sectors into 1 MiB writes. */
#define SECTOR 512U
#define BUCKETS 262144U
#define BATCH 2048U
#define HEADER 512U
#define RECORD (8U + SECTOR + 4U)
#define RECORD_V1 (RECORD + SECTOR)
typedef struct sector Sector;
struct sector { Sector *next; uint64_t off; unsigned char data[SECTOR]; };
static Sector *table[BUCKETS];
static int source = -1;
static uint64_t source_size;
static size_t used, max_sectors;
extern ssize_t __real_pread64(int, void *, size_t, off64_t);
extern ssize_t __real_pwrite64(int, const void *, size_t, off64_t);
extern ssize_t __real_write(int, const void *, size_t);

void fatal(const char *message)
{
    fprintf(stderr, "xfs_deltree: %s%s%s\n", message,
            errno ? ": " : "", errno ? strerror(errno) : "");
    exit(1);
}
static void exact_read(int fd, void *p, size_t n, uint64_t off)
{
    while (n) {
        ssize_t r = __real_pread64(fd, p, n, (off64_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (!r) errno = EIO; fatal("read"); }
        progress_io((uint64_t)r, 0);
        n -= (size_t)r; p = (char *)p + r; off += (uint64_t)r;
    }
}
static void exact_write(int fd, const void *p, size_t n, uint64_t off)
{
    while (n) {
        ssize_t r = __real_pwrite64(fd, p, n, (off64_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (!r) errno = EIO; fatal("write; keep undo journal and recover before mounting"); }
        progress_io(0, (uint64_t)r);
        n -= (size_t)r; p = (const char *)p + r; off += (uint64_t)r;
    }
}
static uint32_t crc(const unsigned char *p, size_t n)
{
    return ~crc32c_le(UINT32_MAX, p, n);
}
static void put64(unsigned char *p, uint64_t x)
{ unsigned i; for (i = 0; i < 8; ++i) p[i] = (unsigned char)(x >> (i * 8)); }
static uint64_t get64(const unsigned char *p)
{ unsigned i; uint64_t x = 0; for (i = 0; i < 8; ++i) x |= (uint64_t)p[i] << (i * 8); return x; }
static void put32(unsigned char *p, uint32_t x)
{ unsigned i; for (i = 0; i < 4; ++i) p[i] = (unsigned char)(x >> (i * 8)); }
static uint32_t get32(const unsigned char *p)
{ unsigned i; uint32_t x = 0; for (i = 0; i < 4; ++i) x |= (uint32_t)p[i] << (i * 8); return x; }
static Sector *lookup(uint64_t off, bool create)
{
    size_t b = (size_t)(((off / SECTOR) * UINT64_C(11400714819323198485)) >> 46);
    Sector *s;
    for (s = table[b]; s; s = s->next) if (s->off == off) return s;
    if (!create) return NULL;
    if (used >= max_sectors) { errno = ENOMEM; fatal("metadata staging limit exceeded; source unchanged"); }
    s = malloc(sizeof *s);
    if (!s) fatal("allocate sector");
    s->off = off; s->next = table[b]; table[b] = s; ++used;
    return s;
}
void stage_begin(int fd, uint64_t size, size_t limit_mb)
{ source = fd; source_size = size; max_sectors = limit_mb * 1024U * 1024U / sizeof(Sector); }
size_t stage_count(void) { return used; }
ssize_t __wrap_pread64(int fd, void *buf, size_t n, off64_t offset)
{
    size_t pos;
    ssize_t r = __real_pread64(fd, buf, n, offset);
    if (fd != source || r <= 0) return r;
    progress_io((uint64_t)r, 0);
    for (pos = 0; pos < (size_t)r;) {
        uint64_t off = (uint64_t)offset + pos, base = off & ~(uint64_t)(SECTOR - 1);
        size_t start = (size_t)(off - base), len = SECTOR - start;
        Sector *s = lookup(base, false);
        if (len > (size_t)r - pos) len = (size_t)r - pos;
        if (s) memcpy((char *)buf + pos, s->data + start, len);
        pos += len;
    }
    return r;
}
ssize_t __wrap_pwrite64(int fd, const void *buf, size_t n, off64_t offset)
{
    size_t pos;
    if (fd != source) return __real_pwrite64(fd, buf, n, offset);
    if (offset < 0 || (uint64_t)offset > source_size || n > source_size - (uint64_t)offset) {
        errno = EINVAL; fatal("out-of-range metadata write");
    }
    for (pos = 0; pos < n;) {
        uint64_t off = (uint64_t)offset + pos, base = off & ~(uint64_t)(SECTOR - 1);
        size_t start = (size_t)(off - base), len = SECTOR - start;
        Sector *s = lookup(base, false);
        if (len > n - pos) len = n - pos;
        if (!s) {
            s = lookup(base, true);
            if (start || len != SECTOR) exact_read(source, s->data, SECTOR, base);
        }
        memcpy(s->data + start, (const char *)buf + pos, len);
        pos += len;
    }
    return (ssize_t)n;
}
ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    if (fd == source) { errno = EPERM; fatal("unexpected sequential source write"); }
    return __real_write(fd, buf, n);
}
static int compare(const void *a, const void *b)
{
    const Sector *x = *(Sector *const *)a, *y = *(Sector *const *)b;
    return (x->off > y->off) - (x->off < y->off);
}
static void sync_parent(const char *path)
{
    char *p = strdup(path), *slash;
    int fd;
    if (!p) fatal("strdup");
    slash = strrchr(p, '/');
    if (slash) { if (slash == p) slash[1] = 0; else *slash = 0; }
    fd = open(slash ? p : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || fsync(fd)) fatal("sync journal directory");
    close(fd); free(p);
}
static void header_crc(unsigned char *h) { put32(h + HEADER - 4, crc(h, HEADER - 4)); }

void stage_apply(const char *journal, const unsigned char uuid[16])
{
    Sector **sorted;
    unsigned char h[HEADER] = {0}, *batch, *records, guard[SECTOR];
    size_t b, i = 0, count;
    int jf;
    struct stat st;
    struct statfs journal_fs;
    const char *fail_after = getenv("XFS_DELTREE_TEST_CRASH_AFTER");
    uint64_t crash_after = fail_after ? strtoull(fail_after, NULL, 10) : UINT64_MAX;
    if (!lookup(0, false)) {
        Sector *first = lookup(0, true);
        exact_read(source, first->data, SECTOR, 0);
    }
    count = used;
    progress_phase("sort staged metadata", NULL, 0);
    sorted = malloc(count * sizeof *sorted);
    batch = malloc(BATCH * SECTOR);
    records = malloc(BATCH * RECORD);
    if (!sorted || !batch || !records) fatal("allocate commit buffers");
    for (b = 0; b < BUCKETS; ++b) {
        Sector *s;
        for (s = table[b]; s; s = s->next) sorted[i++] = s;
    }
    qsort(sorted, count, sizeof *sorted, compare);
    jf = open(journal, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (jf < 0) fatal("create new undo journal");
    if (fstatfs(jf, &journal_fs)) fatal("inspect journal filesystem");
    if (journal_fs.f_type == 0x01021994 || journal_fs.f_type == 0x858458f6) {
        errno = EINVAL; fatal("undo journal must not be on tmpfs or ramfs");
    }
    if (flock(jf, LOCK_EX | LOCK_NB)) fatal("lock journal");
    if (fstat(source, &st)) fatal("stat source");
    memcpy(h, "XFSDTU02", 8); put64(h + 8, source_size);
    memcpy(h + 24, uuid, 16);
    put64(h + 40, S_ISBLK(st.st_mode) ? (uint64_t)st.st_rdev : (uint64_t)st.st_dev);
    put64(h + 48, S_ISBLK(st.st_mode) ? 0 : (uint64_t)st.st_ino);
    h[56] = 1; /* prepared; never written to source before durable journal */
    /* Read original metadata and write undo records in contiguous batches. */
    used = 0;
    progress_phase("write undo journal", "sectors examined", count);
    for (i = 0; i < count;) {
        size_t n = 1, j, packed = 0, previous_used = used;
        while (n < BATCH && i+n < count && sorted[i+n]->off == sorted[i]->off + n*SECTOR) ++n;
        exact_read(source, batch, n*SECTOR, sorted[i]->off);
        for (j = 0; j < n; ++j) {
            Sector *s = sorted[i+j];
            unsigned char *rec = records + packed * RECORD;
            if (s->off && !memcmp(batch+j*SECTOR, s->data, SECTOR)) continue;
            put64(rec, s->off);
            memcpy(rec+8, batch+j*SECTOR, SECTOR);
            put32(rec+RECORD-4, crc(rec, RECORD-4));
            sorted[used++] = s;
            ++packed;
        }
        if (packed) exact_write(jf, records, packed*RECORD, HEADER+(uint64_t)previous_used*RECORD);
        i += n;
        progress_set(i);
    }
    count = used;
    put64(h+16, count); header_crc(h);
    exact_write(jf, h, HEADER, 0);
    progress_phase("sync undo journal", NULL, 0);
    if (fsync(jf)) fatal("sync undo journal; source unchanged");
    sync_parent(journal);
    fprintf(stderr, "Undo journal durable; applying %zu changed sectors\n", count);
    if (crash_after == 0) _exit(99);
    /* An invalid primary magic prevents accidental mounting after a crash.
     * The original superblock is journaled. Restore final magic LAST, only
     * after all other metadata is durable. No XFS feature bit is invented. */
    progress_phase("persist offline guard", NULL, 0);
    exact_read(source, guard, SECTOR, 0);
    memcpy(guard, "XDT!", 4);
    exact_write(source, guard, SECTOR, 0);
    if (fsync(source)) fatal("persist offline guard; recover before mounting");
    progress_phase("apply metadata", "sectors", count);
    progress_set(1);
    for (i = 1; i < count;) {
        size_t n = 1, j;
        while (n < BATCH && i + n < count && sorted[i+n]->off == sorted[i]->off + n * SECTOR) ++n;
        if (crash_after < i + n) n = (size_t)(crash_after - i);
        for (j = 0; j < n; ++j) memcpy(batch + j * SECTOR, sorted[i+j]->data, SECTOR);
        exact_write(source, batch, n * SECTOR, sorted[i]->off);
        i += n;
        progress_set(i);
        if (i >= crash_after) { fsync(source); _exit(99); }
    }
    progress_phase("sync filesystem and final superblock", NULL, 0);
    if (fsync(source)) fatal("sync source; recover before mounting");
    exact_write(source, sorted[0]->data, SECTOR, 0);
    if (fsync(source)) fatal("sync final superblock; recover before mounting");
    /* Append a completion footer instead of rewriting a possibly torn header.
     * A missing/torn footer means rollback remains required and possible. */
    memcpy(h, "DONE0001", 8); header_crc(h);
    exact_write(jf, h, HEADER, HEADER + (uint64_t)count * RECORD);
    if (fsync(jf)) fatal("sync completion footer; keep journal");
    close(jf); free(batch); free(records); free(sorted);
    fprintf(stderr, "Deletion committed. Keep journal until the result is verified.\n");
}

void stage_recover(int fd, const char *journal, uint64_t size)
{
    int jf = open(journal, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    unsigned char h[HEADER], rec[RECORD_V1] = {0}, footer[HEADER], sb[SECTOR];
    struct stat st, js;
    size_t record_size;
    uint64_t count, i, previous = 0, end;
    if (jf < 0 || flock(jf, LOCK_EX | LOCK_NB)) fatal("open/lock recovery journal");
    exact_read(jf, h, HEADER, 0);
    if ((memcmp(h, "XFSDTU01", 8) && memcmp(h, "XFSDTU02", 8)) || get32(h+HEADER-4) != crc(h, HEADER-4) || h[56] != 1)
        { errno = EINVAL; fatal("invalid journal header"); }
    record_size = !memcmp(h, "XFSDTU01", 8) ? RECORD_V1 : RECORD;
    count = get64(h+16);
    if (size < SECTOR || !count || get64(h+8) != size || count > size / SECTOR || count > (UINT64_MAX-2*HEADER)/record_size)
        { errno = EINVAL; fatal("journal size mismatch"); }
    if (fstat(fd, &st) || fstat(jf, &js)) fatal("stat recovery files");
    /* Device major/minor numbers can change across the reboot that prompted
     * recovery. UUID and byte size identify block filesystems; regular images
     * additionally require the original inode, not a different copy. */
    if (get64(h+48) != (S_ISBLK(st.st_mode) ? 0 : (uint64_t)st.st_ino))
        { errno = EINVAL; fatal("journal belongs to another device/image"); }
    exact_read(fd, sb, SECTOR, 0);
    if ((memcmp(sb, "XFSB", 4) && memcmp(sb, "XDT!", 4)) || memcmp(sb+32, h+24, 16))
        { errno = EINVAL; fatal("filesystem UUID mismatch"); }
    end = HEADER + count * record_size;
    if ((uint64_t)js.st_size < end || (uint64_t)js.st_size > end + HEADER)
        { errno = EINVAL; fatal("incomplete or malformed journal; no recovery writes made"); }
    if ((uint64_t)js.st_size == end + HEADER) {
        exact_read(jf, footer, HEADER, end);
        if (get32(footer+HEADER-4) == crc(footer, HEADER-4) && !memcmp(footer+8, h+8, HEADER-12)) {
            if (!memcmp(footer, "DONE0001", 8))
                { errno = EINVAL; fatal("deletion already committed; rollback refused"); }
            if (!memcmp(footer, "BACK0001", 8)) {
                close(jf); fprintf(stderr, "Recovery already completed; no writes made.\n"); return;
            }
        }
    }
    /* Verify ALL records before restoring any sector. Recovery itself can be
     * interrupted and repeated; the journal is retained without modification. */
    progress_phase("verify recovery journal", "records", count);
    for (i = 0; i < count; ++i) {
        uint64_t off;
        exact_read(jf, rec, record_size, HEADER + i*record_size);
        off = get64(rec);
        if (get32(rec+record_size-4) != crc(rec, record_size-4) || off % SECTOR ||
            off > size - SECTOR || (!i && off != 0) || (i && off <= previous))
            { errno = EINVAL; fatal("invalid recovery record; no recovery writes made"); }
        previous = off;
        progress_set(i+1);
    }
    /* Keep the offline guard in place until rollback is fully durable. */
    memcpy(sb, "XDT!", 4);
    exact_write(fd, sb, SECTOR, 0);
    if (fsync(fd)) fatal("sync recovery guard");
    progress_phase("restore original metadata", "sectors", count);
    for (i = 1; i < count; ++i) {
        exact_read(jf, rec, record_size, HEADER + i*record_size);
        exact_write(fd, rec+8, SECTOR, get64(rec));
        progress_set(i);
    }
    progress_phase("sync restored filesystem", NULL, 0);
    if (fsync(fd)) fatal("sync recovery");
    exact_read(jf, rec, record_size, HEADER);
    exact_write(fd, rec+8, SECTOR, 0);
    if (fsync(fd)) fatal("sync restored superblock");
    memcpy(h, "BACK0001", 8); header_crc(h);
    exact_write(jf, h, HEADER, end);
    if (fsync(jf)) fatal("sync recovery completion");
    close(jf);
    fprintf(stderr, "Restored %" PRIu64 " sectors. Recovery journal retained.\n", count);
}
