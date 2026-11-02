/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DELTREE_STAGE_H
#define DELTREE_STAGE_H
#include <stddef.h>
#include <stdint.h>
void stage_begin(int fd, uint64_t size);
void stage_shadow_begin(void);
void stage_shadow_commit(void);
void stage_reset(void);
void stage_end(void);
size_t stage_count(void);
void stage_apply(const char *journal, const unsigned char uuid[16]);
void stage_recover(int fd, const char *journal, uint64_t size);
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void fatal(const char *message);
#endif
