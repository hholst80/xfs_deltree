/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "libxfs.h"

/* Offline deletion never allocates inode clusters. Read/update one filesystem
 * block at a time instead of pulling in the whole (usually 16 KiB) inode
 * cluster for sparse selections. Keep full filesystem blocks so the standard
 * inode-buffer verifier still checks every contained inode. All inode igets
 * share this mapping policy, avoiding overlapping cache aliases.
 * Allocation chunk geometry and on-disk inode locations are unchanged. */
extern int __real_xfs_imap(struct xfs_perag *, struct xfs_trans *,
                             xfs_ino_t, struct xfs_imap *, uint);
int __wrap_xfs_imap(struct xfs_perag *pag, struct xfs_trans *tp,
                      xfs_ino_t ino, struct xfs_imap *imap, uint flags)
{
    struct xfs_mount *mp = pag_mount(pag);
    unsigned block = mp->m_sb.sb_blocksize;
    unsigned skip;
    int error = __real_xfs_imap(pag, tp, ino, imap, flags);
    if (error) return error;
    if ((unsigned)BBTOB(imap->im_len) <= block) return 0;
    skip = imap->im_boffset & ~(block - 1);
    if (skip + block > (unsigned)BBTOB(imap->im_len)) return -EFSCORRUPTED;
    imap->im_blkno += BTOBB(skip);
    imap->im_boffset -= skip;
    imap->im_len = BTOBB(block);
    return 0;
}
