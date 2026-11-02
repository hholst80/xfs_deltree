/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "progress.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>

static const char *mode, *phase, *unit;
static double started, phase_started, last;
static uint64_t done, total, reads, writes, previous_reads, previous_writes;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static double peak_rss_mib(void)
{
    char line[256];
    unsigned long kb;
    struct rusage usage;
    FILE *status = fopen("/proc/self/status", "r");
    /* Linux ru_maxrss can include memory inherited from the launcher before
     * exec. VmHWM describes this executable's address space instead. */
    if (status) {
        while (fgets(line, sizeof line, status)) {
            if (sscanf(line, "VmHWM: %lu kB", &kb) == 1) {
                fclose(status); return (double)kb / 1024.0;
            }
        }
        fclose(status);
    }
    return getrusage(RUSAGE_SELF, &usage) ? 0 : (double)usage.ru_maxrss / 1024.0;
}

static void report(int force)
{
    int saved_errno = errno;
    double t, elapsed, interval;
    if (!mode || !phase) return;
    t = now();
    interval = t - last;
    if (!force && interval < 1.0) { errno = saved_errno; return; }
    elapsed = t - phase_started;
    fprintf(stderr, "[%s +%.1fs] %s", mode, t-started, phase);
    if (unit) {
        double rate = elapsed > 0.0 ? (double)done / elapsed : 0;
        fprintf(stderr, ": %" PRIu64, done);
        if (total) fprintf(stderr, "/%" PRIu64 " (%.1f%%)", total, 100.0*(double)done/(double)total);
        fprintf(stderr, " %s; avg %.0f %s/s", unit, rate, unit);
        if (total && done && done < total && elapsed >= 1.0)
            fprintf(stderr, "; ETA ~%.0fs", (double)(total-done)/rate);
    }
    fprintf(stderr, "; read=%.1f MiB", (double)reads/1048576.0);
    if (interval >= 0.1) fprintf(stderr, " (%.1f MiB/s)", (double)(reads-previous_reads)/1048576.0/interval);
    fprintf(stderr, "; written=%.1f MiB", (double)writes/1048576.0);
    if (interval >= 0.1) fprintf(stderr, " (%.1f MiB/s)", (double)(writes-previous_writes)/1048576.0/interval);
    fprintf(stderr, "; peak-RSS=%.1f MiB", peak_rss_mib());
    fputc('\n', stderr);
    last = t; previous_reads = reads; previous_writes = writes;
    errno = saved_errno;
}

void progress_start(const char *run_mode)
{
    mode = run_mode;
    started = last = now();
}
void progress_phase(const char *name, const char *units, uint64_t count)
{
    if (phase) report(1);
    phase = name; unit = units; total = count; done = 0;
    phase_started = now();
    report(1);
}
void progress_set(uint64_t count) { done = count; report(0); }
void progress_io(uint64_t r, uint64_t w) { reads += r; writes += w; report(0); }
void progress_finish(void) { report(1); phase = NULL; }
