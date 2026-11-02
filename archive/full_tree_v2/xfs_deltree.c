/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "libxfs.h"
#include "libxlog.h"
#include "walker.h"
#include "stage.h"
#include "progress.h"
#include "bulk_inode.h"
#include "bulk_extent.h"
#include <dirent.h>
#include <getopt.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>

/* Single-threaded C99 application; libxfs has internal pthread dependencies.
 * No unlink/unlinkat/rmdir calls. Namespace discovery precedes all mutations. */
#define INO_BUCKETS 262144U
typedef struct entry Entry;
struct entry {
    xfs_ino_t ino, parent;
    uint64_t refs, subdirs;
    mode_t mode;
    uint8_t dtype;
    Entry *next;
};
static Entry *buckets[INO_BUCKETS], **entries;
static size_t nr, capacity;
static size_t inode_limit;
static struct xfs_mount mount_state, *mp;
static struct libxfs_init xi;
static xfs_ino_t root_ino, parent_ino;
static uint64_t freed_inodes, retained_inodes;
static char *target_name;

static double seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int xlog_recover_do_trans(struct xlog *log, struct xlog_recover *t, int pass)
{ (void)log; (void)t; (void)pass; return -EOPNOTSUPP; }

static void check(int error, const char *operation)
{ if (error) { errno = error < 0 ? -error : error; fatal(operation); } }
static void reject(const char *message) { errno = 0; fatal(message); }

static Entry *record(xfs_ino_t ino, xfs_ino_t parent)
{
    size_t b = (size_t)((ino * UINT64_C(11400714819323198485)) >> 46);
    Entry *e;
    for (e = buckets[b]; e; e = e->next) {
        if (e->ino == ino) { ++e->refs; return e; }
    }
    if (nr >= inode_limit) reject("inode-index memory limit reached; source unchanged");
    if (nr == capacity) {
        Entry **new_entries;
        capacity = capacity ? capacity * 2 : 4096;
        new_entries = realloc(entries, capacity * sizeof *entries);
        if (!new_entries) fatal("allocate inode index");
        entries = new_entries;
    }
    e = calloc(1, sizeof *e);
    if (!e) fatal("allocate inode record");
    e->ino = ino; e->parent = parent; e->refs = 1;
    e->next = buckets[b]; buckets[b] = e; entries[nr++] = e;
    progress_set(nr);
    return e;
}

static int collect(struct xfs_trans *tp, struct xfs_inode *dp,
    xfs_dir2_dataptr_t off, char *name, ssize_t len, xfs_ino_t ino,
    uint8_t dtype, void *private)
{
    Entry *dir = private;
    (void)tp; (void)off;
    if (len < 0) len = (ssize_t)strlen(name);
    if (len == 1 && name[0] == '.') return ino == dp->i_ino ? 0 : EFSCORRUPTED;
    if (len == 2 && name[0] == '.' && name[1] == '.') return ino == dir->parent ? 0 : EFSCORRUPTED;
    if (len <= 0 || len > 255 || memchr(name, '/', (size_t)len) || memchr(name, 0, (size_t)len) ||
        !xfs_verify_ino(mp, ino) || ino == mp->m_sb.sb_rootino || ino == parent_ino)
        return EFSCORRUPTED;
    if (dtype == XFS_DIR3_FT_DIR) ++dir->subdirs;
    {
        Entry *e = record(ino, dp->i_ino);
        if (e->dtype && dtype && e->dtype != dtype) return EFSCORRUPTED;
        if (dtype) e->dtype = dtype;
    }
    return 0;
}

static xfs_ino_t resolve(const char *path)
{
    char *copy = strdup(path), *save = NULL, *part, *next;
    xfs_ino_t ino = mp->m_sb.sb_rootino;
    if (!copy) fatal("allocate path");
    part = strtok_r(copy, "/", &save);
    if (!part) reject("refusing filesystem root");
    while (part) {
        struct xfs_inode *ip;
        struct xfs_name name;
        if (!strcmp(part, ".") || !strcmp(part, "..")) reject("dot path components are not allowed");
        next = strtok_r(NULL, "/", &save);
        check(libxfs_iget(mp, NULL, ino, 0, &ip), "load path inode");
        if (!S_ISDIR(VFS_I(ip)->i_mode)) reject("path traverses a non-directory or symlink");
        if (!next) {
            parent_ino = ino;
            target_name = strdup(part);
            if (!target_name) fatal("allocate target name");
        }
        memset(&name, 0, sizeof name);
        name.name = (const unsigned char *)part; name.len = strlen(part);
        check(libxfs_dir_lookup(NULL, ip, &name, &ino, NULL), "lookup path");
        libxfs_irele(ip);
        part = next;
    }
    free(copy);
    if (ino == mp->m_sb.sb_rootino) reject("refusing filesystem root");
    return ino;
}

static void discover(void)
{
    size_t i;
    progress_phase("scan directories", "inodes found", 0);
    record(root_ino, parent_ino)->dtype = XFS_DIR3_FT_DIR;
    for (i = 0; i < nr; ++i) {
        Entry *e = entries[i];
        struct xfs_inode *ip;
        /* Ordinary files are verified when preparing their inode update.
         * Only directories need inode decoding during namespace discovery. */
        if (e->dtype && e->dtype != XFS_DIR3_FT_DIR) continue;
        check(libxfs_iget(mp, NULL, e->ino, 0, &ip), "read inode during scan");
        e->mode = VFS_I(ip)->i_mode;
        if (!e->mode || !VFS_I(ip)->i_nlink || VFS_I(ip)->i_nlink == XFS_NLINK_PINNED)
            reject("free, orphaned, or pinned-link inode encountered");
        if (ip->i_diflags & (XFS_DIFLAG_IMMUTABLE | XFS_DIFLAG_APPEND | XFS_DIFLAG_REALTIME))
            reject("immutable, append-only or realtime inode encountered");
        if (e->ino == root_ino && !S_ISDIR(e->mode)) reject("target must be a directory");
        if (S_ISDIR(e->mode)) {
            if (e->refs != 1) reject("directory cycle or multiple directory parents");
            check(listdir(NULL, ip, collect, e), "scan directory");
            if (VFS_I(ip)->i_nlink != e->subdirs + 2) reject("directory link count does not match entries");
        }
        libxfs_irele(ip);
        progress_set(nr);
    }
    for (i = 0; i < nr; ++i)
        if (S_ISDIR(entries[i]->mode) && entries[i]->refs != 1)
            reject("directory cycle or multiple directory parents");
    fprintf(stderr, "Scanned %zu unique inodes; preparing metadata changes\n", nr);
}

static void detach_root(void)
{
    struct xfs_inode *dp;
    struct xfs_trans *tp;
    struct xfs_name name;
    unsigned resblks = libxfs_remove_space_res(mp, strlen(target_name));
    check(libxfs_iget(mp, NULL, parent_ino, 0, &dp), "load target parent");
    if (!S_ISDIR(VFS_I(dp)->i_mode) || VFS_I(dp)->i_nlink < 3 ||
        dp->i_diflags & (XFS_DIFLAG_IMMUTABLE | XFS_DIFLAG_APPEND))
        reject("invalid or protected target parent");
    check(libxfs_trans_alloc(mp, &M_RES(mp)->tr_remove, resblks, 0, 0, &tp), "allocate parent transaction");
    libxfs_trans_ijoin(tp, dp, 0);
    memset(&name, 0, sizeof name);
    name.name = (const unsigned char *)target_name; name.len = strlen(target_name);
    check(libxfs_dir_removename(tp, dp, &name, root_ino, resblks), "remove target entry from its parent");
    drop_nlink(VFS_I(dp));
    libxfs_trans_ichgtime(tp, dp, XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
    libxfs_trans_log_inode(tp, dp, XFS_ILOG_CORE);
    check(libxfs_trans_commit(tp), "commit parent metadata to staging");
    libxfs_irele(dp);
}

static void free_fork(struct xfs_inode *ip, int whichfork)
{
    struct xfs_ifork *ifp = xfs_ifork_ptr(ip, whichfork);
    struct xfs_trans *tp;
    xfs_fileoff_t last;
    int done = 0;
    int flags = whichfork == XFS_ATTR_FORK ? XFS_BMAPI_ATTRFORK | XFS_BMAPI_METADATA :
        S_ISREG(VFS_I(ip)->i_mode) ? 0 : XFS_BMAPI_METADATA;
    if (!ifp || ifp->if_format == XFS_DINODE_FMT_LOCAL || ifp->if_format == XFS_DINODE_FMT_DEV)
        return;
    check(libxfs_bmap_last_offset(ip, &last, whichfork), "find fork extent limit");
    if (!last) return;
    check(libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0, &tp), "allocate extent transaction");
    libxfs_trans_ijoin(tp, ip, 0);
    while (!done) {
        check(libxfs_bunmapi(tp, ip, 0, last, flags, 0, &done), "free inode extents");
        check(libxfs_defer_finish(&tp), "finish extent/refcount updates");
        check(libxfs_trans_roll_inode(&tp, ip), "roll extent transaction");
    }
    check(libxfs_trans_commit(tp), "commit extents to staging");
}

static int order_inodes(const void *a, const void *b)
{
    xfs_ino_t x = (*(Entry *const *)a)->ino, y = (*(Entry *const *)b)->ino;
    return (x > y) - (x < y);
}

static void mutate(void)
{
    size_t i = 0;
    double loadtime = 0, alloctime = 0, coretime = 0, tick;
    progress_phase("prepare inode changes", "inodes", nr);
    detach_root();
    qsort(entries, nr, sizeof *entries, order_inodes);
    while (i < nr) {
        struct xfs_inode *loaded[64];
        bool remove[64];
        size_t end = i, j;
        xfs_ino_t chunk = inode_chunk_start(mp, entries[i]->ino);
        uint64_t mask = 0;
        struct xfs_trans *tp;
        while (end < nr && entries[end]->ino >= chunk && entries[end]->ino - chunk < 64) ++end;
        tick = seconds();
        /* Finish extent operations before joining inodes to the shared final
         * transaction; those operations can roll their own transactions. */
        for (j = i; j < end; ++j) {
            Entry *e = entries[j];
            struct xfs_inode *ip;
            check(libxfs_iget(mp, NULL, e->ino, 0, &ip), "load inode for deletion");
            loaded[j-i] = ip;
            if (!VFS_I(ip)->i_mode || !VFS_I(ip)->i_nlink || VFS_I(ip)->i_nlink == XFS_NLINK_PINNED)
                reject("free, orphaned or pinned-link inode encountered");
            if (ip->i_diflags & (XFS_DIFLAG_IMMUTABLE | XFS_DIFLAG_APPEND | XFS_DIFLAG_REALTIME))
                reject("immutable, append-only or realtime inode encountered");
            if (!!S_ISDIR(VFS_I(ip)->i_mode) != !!S_ISDIR(e->mode))
                reject("directory entry type disagrees with inode");
            if (e->refs > VFS_I(ip)->i_nlink) reject("removed references exceed inode link count");
            remove[j-i] = S_ISDIR(e->mode) || e->refs == VFS_I(ip)->i_nlink;
            if (remove[j-i]) {
                free_fork(ip, XFS_ATTR_FORK);
                if (!bulk_data_eligible(ip)) free_fork(ip, XFS_DATA_FORK);
                mask |= UINT64_C(1) << (e->ino - chunk);
            }
        }
        loadtime += seconds() - tick; tick = seconds();
        check(libxfs_trans_alloc(mp, &M_RES(mp)->tr_ifree, XFS_IFREE_SPACE_RES(mp), 0, 0, &tp), "allocate inode batch transaction");
        /* Deferred allocation/rmap work can roll a transaction, whose held
         * inode array is small. Complete it before joining the inode batch. */
        for (j = i; j < end; ++j)
            if (remove[j-i] && bulk_data_eligible(loaded[j-i]))
                bulk_data_collect(tp, loaded[j-i], inode_limit);
        free_inode_mask(tp, chunk, mask);
        check(libxfs_trans_commit(tp), "commit allocation batch to staging");
        alloctime += seconds() - tick; tick = seconds();
        check(libxfs_trans_alloc(mp, &M_RES(mp)->tr_ifree, 0, 0, 0, &tp), "allocate inode core transaction");
        for (j = i; j < end; ++j) {
            Entry *e = entries[j];
            struct xfs_inode *ip = loaded[j-i];
            libxfs_trans_ijoin(tp, ip, 0);
            if (!remove[j-i]) {
                set_nlink(VFS_I(ip), VFS_I(ip)->i_nlink - e->refs);
                libxfs_trans_ichgtime(tp, ip, XFS_ICHGTIME_CHG);
                ++retained_inodes;
            } else {
                /* All namespace changes stay in the overlay until the complete
                 * preimage journal is durable, so no transient orphan list is
                 * needed. Keep empty inode clusters allocated for future reuse. */
                libxfs_idestroy_fork(&ip->i_df);
                ip->i_df.if_bytes = 0;
                libxfs_ifork_zap_attr(ip);
                VFS_I(ip)->i_mode = 0; set_nlink(VFS_I(ip), 0);
                VFS_I(ip)->i_generation++;
                ip->i_diflags = 0; ip->i_diflags2 = mp->m_ino_geo.new_diflags2;
                ip->i_forkoff = 0; ip->i_df.if_format = XFS_DINODE_FMT_EXTENTS;
                ip->i_disk_size = 0;
                if (ip->i_nblocks || ip->i_df.if_nextents) reject("inode still owns blocks after freeing forks");
                ++freed_inodes;
            }
            libxfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
        }
        check(libxfs_trans_commit(tp), "commit inode batch to staging");
        for (j = i; j < end; ++j) libxfs_irele(loaded[j-i]);
        coretime += seconds() - tick;
        progress_set(end);
        i = end;
    }
    tick = seconds();
    bulk_data_free(mp);
    fprintf(stderr, "Prepare phases: load/forks=%.6f allocation=%.6f cores=%.6f bulk-free=%.6f\n", loadtime, alloctime, coretime, seconds()-tick);
}

/* Regular images must not be attached to a loop device, mounted or otherwise.
 * Holding an exclusive flock also prevents concurrent instances of this tool.
 * Block devices use the kernel's O_EXCL claim via libxfs. */
static void reject_loop_image(const struct stat *st)
{
    DIR *d = opendir("/sys/block");
    struct dirent *de;
    if (!d) fatal("inspect loop devices");
    while ((de = readdir(d))) {
        char path[320];
        int fd;
        struct loop_info64 info;
        if (strncmp(de->d_name, "loop", 4)) continue;
        snprintf(path, sizeof path, "/dev/%s", de->d_name);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { if (errno == ENOENT) continue; fatal("inspect loop device"); }
        if (!ioctl(fd, LOOP_GET_STATUS64, &info) && info.lo_device == (uint64_t)st->st_dev &&
            info.lo_inode == (uint64_t)st->st_ino) reject("image is attached to a loop device; detach it first");
        close(fd);
    }
    closedir(d);
}

static uint64_t device_size(int fd, const struct stat *st)
{
    uint64_t size;
    if (S_ISREG(st->st_mode)) return (uint64_t)st->st_size;
    if (ioctl(fd, BLKGETSIZE64, &size)) fatal("read device size");
    return size;
}

static void usage(void)
{
    puts("Usage: xfs_deltree [--dry-run | --journal FILE] [--memory-mb N] DEVICE /PATH-IN-XFS\n"
         "       xfs_deltree --recover JOURNAL DEVICE\n"
         "Offline bulk XFS metadata deletion. DEVICE must be unmounted and exclusive.\n"
         "Default is --dry-run. Writes require a new undo journal on another filesystem.\n"
         "Supports clean XFS v5 with internal log; no active quotas, realtime, parent\n"
         "pointers or metadata directories. Paths are relative to the filesystem root.\n"
         "After interrupted application, recover BEFORE mounting or using the device.");
}

int main(int argc, char **argv)
{
    double t0 = seconds(), t1, t2, t3, t4;
    static const struct option opts[] = {
        {"dry-run", no_argument, NULL, 'n'}, {"journal", required_argument, NULL, 'j'},
        {"recover", required_argument, NULL, 'r'}, {"memory-mb", required_argument, NULL, 'm'},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    const char *journal = NULL, *recover = NULL, *device;
    bool explicit_dry = false;
    size_t memory_mb = 1024;
    int opt, fd, result;
    uint64_t size;
    struct stat st;
    struct xfs_buf *bp;
    struct xlog log;
    unsigned char uuid[16];
    while ((opt = getopt_long(argc, argv, "nj:r:m:h", opts, NULL)) != -1) {
        switch (opt) {
        case 'n': explicit_dry = true; break;
        case 'j': journal = optarg; break;
        case 'r': recover = optarg; break;
        case 'm': {
            char *end; unsigned long v;
            errno = 0; v = strtoul(optarg, &end, 10);
            if (errno || !*optarg || *end || v < 16 || v > 65536) reject("memory-mb must be 16..65536");
            memory_mb = v; break;
        }
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }
    if (argc - optind != (recover ? 1 : 2) || (recover && (journal || explicit_dry)) || (journal && explicit_dry))
        { usage(); return 2; }
    progress_start(recover ? "recovery" : journal ? "delete" : "dry-run");
    progress_phase("open and validate filesystem", NULL, 0);
    device = argv[optind];
    inode_limit = memory_mb * 1024U * 1024U / (sizeof(Entry) + 2 * sizeof(Entry *) + 16);
    if (stat(device, &st)) fatal("stat source");
    if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode)) reject("source must be a block device or filesystem image");
    if (S_ISREG(st.st_mode)) reject_loop_image(&st);
    if (recover) {
        fd = open(device, O_RDWR | O_CLOEXEC | (S_ISBLK(st.st_mode) ? O_EXCL : 0));
        if (fd < 0 || (S_ISREG(st.st_mode) && flock(fd, LOCK_EX | LOCK_NB))) fatal("claim recovery device exclusively");
        stage_recover(fd, recover, device_size(fd, &st));
        progress_finish();
        close(fd); return 0;
    }
    progname = "xfs_deltree";
    xi.data.name = (char *)device; xi.data.isfile = S_ISREG(st.st_mode);
    xi.flags = LIBXFS_EXCLUSIVELY;
    if (!libxfs_init(&xi)) reject("cannot initialize libxfs; source must be unmounted");
    fd = xi.data.fd;
    if (S_ISREG(st.st_mode) && flock(fd, LOCK_EX | LOCK_NB)) fatal("lock source");
    size = device_size(fd, &st);
    stage_begin(fd, size, memory_mb);
    libxfs_buftarg_init(&mount_state, &xi);
    check(libxfs_buf_read_uncached(mount_state.m_ddev_targp, XFS_SB_DADDR,
        1, 0, &bp, NULL), "read XFS superblock geometry");
    libxfs_sb_from_disk(&mount_state.m_sb, bp->b_addr);
    libxfs_buf_relse(bp);
    if (mount_state.m_sb.sb_magicnum == 0x58445421)
        reject("unfinished offline update: use --recover JOURNAL DEVICE before mounting");
    if (mount_state.m_sb.sb_magicnum != XFS_SB_MAGIC || mount_state.m_sb.sb_sectlog < 9 ||
        mount_state.m_sb.sb_sectlog > XFS_MAX_SECTORSIZE_LOG)
        reject("invalid XFS superblock geometry");
    check(libxfs_buf_read_uncached(mount_state.m_ddev_targp, XFS_SB_DADDR,
        1 << (mount_state.m_sb.sb_sectlog - BBSHIFT), 0, &bp, &xfs_sb_buf_ops), "verify XFS superblock");
    libxfs_sb_from_disk(&mount_state.m_sb, bp->b_addr);
    libxfs_buf_relse(bp);
    if (!xfs_sb_version_hascrc(&mount_state.m_sb) || !mount_state.m_sb.sb_logstart ||
        mount_state.m_sb.sb_rblocks || mount_state.m_sb.sb_qflags || mount_state.m_sb.sb_inprogress)
        reject("requires XFS v5, internal log, no realtime/active quota, completed mkfs");
    if (mount_state.m_sb.sb_dblocks > size / mount_state.m_sb.sb_blocksize)
        reject("filesystem exceeds device size");
    mp = libxfs_mount(&mount_state, &mount_state.m_sb, &xi, 0);
    if (!mp) reject("cannot initialize XFS geometry");
    if (!xfs_has_ftype(mp) || xfs_has_parent(mp) || xfs_has_metadir(mp) || xfs_has_zoned(mp))
        reject("unsupported XFS feature: missing ftype, parent pointers, metadir or zoned");
    progress_phase("check XFS log", NULL, 0);
    result = xlog_is_dirty(mp, &log);
    if (result) reject("log is dirty or unreadable; replay it by mounting, then cleanly unmount");
    libxfs_max_lsn = atomic64_read(&log.l_last_sync_lsn);
    check(libxfs_initialize_perag_data(mp, mp->m_sb.sb_agcount), "initialize allocation group accounting");
    memcpy(uuid, &mp->m_sb.sb_uuid, 16);
    progress_phase("resolve target path", NULL, 0);
    root_ino = resolve(argv[optind + 1]);
    t1 = seconds();
    discover();
    t2 = seconds();
    mutate();
    progress_phase("flush metadata to memory overlay", NULL, 0);
    check(libxfs_umount(mp), "flush metadata to staging");
    t3 = seconds();
    fprintf(stderr, "Plan: free=%" PRIu64 " preserve-external-links=%" PRIu64 " staged-sectors=%zu\n",
        freed_inodes, retained_inodes, stage_count());
    if (journal) stage_apply(journal, uuid);
    else fprintf(stderr, "Dry run: source unchanged. Supply --journal FILE to apply.\n");
    progress_finish();
    t4 = seconds();
    fprintf(stderr, "Timing: setup=%.6f scan=%.6f prepare=%.6f apply=%.6f seconds\n",
        t1-t0, t2-t1, t3-t2, t4-t3);
    libxfs_destroy(&xi);
    return 0;
}
