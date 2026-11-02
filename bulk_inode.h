/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DELTREE_BULK_INODE_H
#define DELTREE_BULK_INODE_H
xfs_ino_t inode_chunk_start(struct xfs_mount *mp, xfs_ino_t ino);
void free_inode_mask(struct xfs_trans *tp, xfs_ino_t first, uint64_t mask);
#endif
