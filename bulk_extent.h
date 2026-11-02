/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DELTREE_BULK_EXTENT_H
#define DELTREE_BULK_EXTENT_H
bool bulk_data_eligible(struct xfs_inode *ip);
void bulk_data_collect(struct xfs_trans *tp, struct xfs_inode *ip, size_t limit);
void bulk_data_free(struct xfs_mount *mp);
#endif
