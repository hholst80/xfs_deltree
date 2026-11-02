#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/openat2.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Each directory has one scanner. Its scan token and child tokens keep its
 * descriptor alive. The last token holder removes it and releases its parent.
 * All token/queue operations use mu; directory streams have a single reader. */
typedef struct node Node;
struct node {
    Node *parent, *next, *resume;
    DIR *dir;
    char *name;
    dev_t dev;
    ino_t ino;
    size_t pending;
    bool failed;
};
typedef struct {
    uint64_t files, dirs, errors;
} Stats;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static Node *head, *tail;
static size_t queued, queue_limit;
static bool done;
static int root_parent;
static volatile sig_atomic_t interrupted;

static void on_signal(int signo) { interrupted = signo; }

/* Print untrusted names without allowing terminal-control characters. */
static void quote_name(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    fputc('\'', stderr);
    for (; *p; ++p) {
        if (*p >= 32 && *p < 127 && *p != '\'' && *p != '\\')
            fputc(*p, stderr);
        else
            fprintf(stderr, "\\x%02x", (unsigned)*p);
    }
    fputc('\'', stderr);
}

static void report(Node *n, const char *name, const char *op, int err, Stats *s)
{
    ++s->errors;
    pthread_mutex_lock(&mu);
    if (n) n->failed = true;
    pthread_mutex_unlock(&mu);
    flockfile(stderr);
    fprintf(stderr, "xfs_deltree: %s ", op);
    quote_name(name);
    if (n) fprintf(stderr, " (directory inode %ju)", (uintmax_t)n->ino);
    fprintf(stderr, ": %s\n", strerror(err));
    funlockfile(stderr);
}

static int open_dir(int parent, const char *name, bool confined)
{
    struct open_how how = {0};
    how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_NO_SYMLINKS;
    if (confined) how.resolve |= RESOLVE_BENEATH | RESOLVE_NO_XDEV;
    return (int)syscall(SYS_openat2, parent, name, &how, sizeof how);
}

static Node *new_node(Node *parent, const char *name, int fd)
{
    struct stat st;
    Node *n;
    int saved;
    if (fstat(fd, &st) < 0) goto fail_fd;
    n = calloc(1, sizeof *n);
    if (!n) goto fail_fd;
    n->name = strdup(name);
    if (!n->name) { free(n); goto fail_fd; }
    n->dir = fdopendir(fd);
    if (!n->dir) { free(n->name); free(n); goto fail_fd; }
    n->parent = parent;
    n->dev = st.st_dev;
    n->ino = st.st_ino;
    n->pending = 1;
    return n;
fail_fd:
    saved = errno;
    close(fd);
    errno = saved;
    return NULL;
}

static void enqueue(Node *n)
{
    n->next = NULL;
    if (tail) tail->next = n;
    else head = n;
    tail = n;
    ++queued;
    pthread_cond_signal(&ready);
}

/* Called once per completed scan/child, including errors and cancellation. */
static void release_node(Node *n, bool child_failed, Stats *s)
{
    while (n) {
        Node *parent;
        struct stat st;
        int pfd;
        bool failed;
        pthread_mutex_lock(&mu);
        n->failed |= child_failed;
        if (--n->pending) { pthread_mutex_unlock(&mu); return; }
        failed = n->failed;
        pthread_mutex_unlock(&mu);
        parent = n->parent;
        pfd = parent ? dirfd(parent->dir) : root_parent;
        if (!failed && !interrupted) {
            /* Detect replacements before removing the name. This is not an
             * atomic compare-and-unlink; hostile renames require isolation. */
            if (fstatat(pfd, n->name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
                if (errno != ENOENT) {
                    report(n, n->name, "check directory", errno, s);
                    failed = true;
                }
            } else if (st.st_dev != n->dev || st.st_ino != n->ino) {
                report(n, n->name, "directory replaced", ESTALE, s);
                failed = true;
            } else if (unlinkat(pfd, n->name, AT_REMOVEDIR) == 0) {
                ++s->dirs;
            } else if (errno != ENOENT) {
                report(n, n->name, "remove directory", errno, s);
                failed = true;
            }
        }
        closedir(n->dir);
        free(n->name);
        free(n);
        if (!parent) {
            pthread_mutex_lock(&mu);
            done = true;
            pthread_cond_broadcast(&ready);
            pthread_mutex_unlock(&mu);
        }
        n = parent;
        child_failed = failed || interrupted;
    }
}

static void scan(Node *n, Stats *s)
{
    while (n) {
        struct dirent *de;
        Node *child, *resume;
        int fd, err;
        bool is_dir;
        if (interrupted) goto finish;
        errno = 0;
        de = readdir(n->dir);
        if (!de) {
            if (errno) report(n, n->name, "read directory", errno, s);
            goto finish;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        is_dir = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN) {
            struct stat st;
            if (fstatat(dirfd(n->dir), de->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
                if (errno != ENOENT) report(n, de->d_name, "stat", errno, s);
                continue;
            }
            is_dir = S_ISDIR(st.st_mode);
        }
        if (!is_dir) {
            if (!unlinkat(dirfd(n->dir), de->d_name, 0)) ++s->files;
            else if (errno != ENOENT) report(n, de->d_name, "unlink", errno, s);
            continue;
        }
        fd = open_dir(dirfd(n->dir), de->d_name, true);
        if (fd < 0) {
            err = errno;
            /* An unreadable but empty directory can still be removable.
             * AT_REMOVEDIR cannot traverse a symlink or remove a mount. */
            if (err == EACCES && !unlinkat(dirfd(n->dir), de->d_name, AT_REMOVEDIR))
                ++s->dirs;
            else if (err != ENOENT) report(n, de->d_name, "open directory", err, s);
            continue;
        }
        child = new_node(n, de->d_name, fd);
        if (!child) { report(n, de->d_name, "allocate directory", errno, s); continue; }
        pthread_mutex_lock(&mu);
        ++n->pending;
        if (queued < queue_limit) {
            enqueue(child);
            pthread_mutex_unlock(&mu);
        } else {
            pthread_mutex_unlock(&mu);
            /* Explicit stack: no C recursion and no waiting on a full queue. */
            child->resume = n;
            n = child;
        }
        continue;
finish:
        resume = n->resume;
        release_node(n, interrupted != 0, s);
        n = resume;
    }
}

static void *worker(void *arg)
{
    Stats *s = arg;
    for (;;) {
        Node *n;
        pthread_mutex_lock(&mu);
        while (!head && !done) pthread_cond_wait(&ready, &mu);
        if (done) { pthread_mutex_unlock(&mu); return NULL; }
        n = head;
        head = n->next;
        if (!head) tail = NULL;
        --queued;
        pthread_mutex_unlock(&mu);
        scan(n, s);
    }
}

static void usage(FILE *out)
{
    fprintf(out, "Usage: xfs_deltree [-j WORKERS] [--stats] -- DIRECTORY\n"
        "Delete DIRECTORY and all descendants. Default: 4 workers; -j 1 is serial.\n"
        "Linux 5.6+ with openat2 required; C99, libc and pthreads only.\n"
        "Refuses symlinks in the operand, mount-point roots, /, . and ...\n"
        "Does not cross descendant mounts or follow descendant symlinks.\n"
        "Requires a tree without concurrent renames for strict scope guarantees.\n"
        "Exit: 0 complete, 1 incomplete/error, 2 usage, 128+signal interrupted.\n");
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        {"jobs", required_argument, NULL, 'j'},
        {"stats", no_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    unsigned jobs = 4, started = 0, i;
    int c, fd, rc;
    bool stats = false;
    char *path, *slash, *name, *parent_path;
    size_t len;
    Node *root;
    pthread_t threads[63];
    Stats counts[64] = {{0}}, total = {0};
    struct stat st_root;
    struct timespec begin, end;
    struct sigaction sa;
    while ((c = getopt_long(argc, argv, "j:sh", opts, NULL)) != -1) {
        if (c == 'h') { usage(stdout); return 0; }
        if (c == 's') { stats = true; continue; }
        if (c == 'j') {
            char *stop;
            unsigned long v;
            errno = 0;
            v = strtoul(optarg, &stop, 10);
            if (errno || !*optarg || *stop || v < 1 || v > 64) {
                fprintf(stderr, "xfs_deltree: workers must be 1..64\n"); return 2;
            }
            jobs = (unsigned)v;
        } else { usage(stderr); return 2; }
    }
    if (optind != argc - 1 || !argv[optind][0]) { usage(stderr); return 2; }
    path = strdup(argv[optind]);
    if (!path) { perror("strdup"); return 1; }
    len = strlen(path);
    while (len > 1 && path[len - 1] == '/') path[--len] = 0;
    slash = strrchr(path, '/');
    name = slash ? slash + 1 : path;
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) {
        fprintf(stderr, "xfs_deltree: refusing root, . or .. operand\n"); free(path); return 2;
    }
    parent_path = ".";
    if (slash) { *slash = 0; parent_path = slash == path ? "/" : path; }
    root_parent = open_dir(AT_FDCWD, parent_path, false);
    if (root_parent < 0) { perror("xfs_deltree: open parent (openat2 required)"); free(path); return 1; }
    fd = open_dir(root_parent, name, true);
    if (fd < 0) {
        perror("xfs_deltree: open target (must be a directory, not a mount or symlink)");
        close(root_parent); free(path); return 1;
    }
    root = new_node(NULL, name, fd);
    free(path);
    if (!root) { perror("xfs_deltree: allocate root"); close(root_parent); return 1; }
    if (stat("/", &st_root) || (root->dev == st_root.st_dev && root->ino == st_root.st_ino)) {
        fprintf(stderr, "xfs_deltree: cannot validate target or target is filesystem root\n");
        closedir(root->dir); free(root->name); free(root); close(root_parent); return 1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL)) {
        perror("sigaction"); closedir(root->dir); free(root->name); free(root); close(root_parent); return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &begin);
    queue_limit = jobs == 1 ? 0 : jobs * 2;
    enqueue(root);
    for (i = 1; i < jobs; ++i) {
        rc = pthread_create(&threads[started], NULL, worker, &counts[i]);
        if (rc) { fprintf(stderr, "xfs_deltree: fewer workers: %s\n", strerror(rc)); break; }
        ++started;
    }
    worker(&counts[0]);
    for (i = 0; i < started; ++i) pthread_join(threads[i], NULL);
    close(root_parent);
    clock_gettime(CLOCK_MONOTONIC, &end);
    for (i = 0; i < jobs; ++i) {
        total.files += counts[i].files;
        total.dirs += counts[i].dirs;
        total.errors += counts[i].errors;
    }
    if (stats || total.errors || interrupted) {
        double secs = (double)(end.tv_sec - begin.tv_sec) + (double)(end.tv_nsec - begin.tv_nsec) / 1e9;
        fprintf(stderr, "xfs_deltree: files=%" PRIu64 " directories=%" PRIu64
            " errors=%" PRIu64 " seconds=%.3f workers=%u%s\n",
            total.files, total.dirs, total.errors, secs, started + 1,
            interrupted ? " interrupted" : "");
    }
    return interrupted ? 128 + interrupted : total.errors ? 1 : 0;
}
