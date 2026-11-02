/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DELTREE_PROGRESS_H
#define DELTREE_PROGRESS_H
#include <stdint.h>
void progress_start(const char *mode);
void progress_phase(const char *name, const char *unit, uint64_t total);
void progress_set(uint64_t done);
void progress_io(uint64_t read_bytes, uint64_t written_bytes);
void progress_finish(void);
#endif
