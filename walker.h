/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DELTREE_WALKER_H
#define DELTREE_WALKER_H
typedef int (*dir_emit_t)(struct xfs_trans *, struct xfs_inode *,
    xfs_dir2_dataptr_t, char *, ssize_t, xfs_ino_t, uint8_t, void *);
int listdir(struct xfs_trans *, struct xfs_inode *, dir_emit_t, void *);
#endif
