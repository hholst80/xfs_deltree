/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DELTREE_CONFIG_H
#define DELTREE_CONFIG_H
/* Working-set target: about 128 MiB, independent of tree population.
 * These are guards, not a process-wide RSS guarantee for arbitrary geometry. */
#define DELTREE_STAGE_MB 64U
#define DELTREE_CHECKPOINT_SECTORS (16U * 1024U * 1024U / 512U)
#define DELTREE_SUBTREE_INODES 32768U
#define DELTREE_BATCH_INODES 32768U
#define DELTREE_SMALL_RMAP_MB 32U
#define DELTREE_LARGE_BATCH_DIVISOR 4U
#define DELTREE_EXTENTS 65536U
#define DELTREE_FORK_EXTENTS 8192U
#define DELTREE_CACHE_BUCKETS 64
#define DELTREE_SHADOW_CACHE_KB 4096
#endif
