/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "libxfs.h"
#include "stage.h"
#include "bulk_inode.h"

static void need(int error, const char *what)
{
    if (error) { errno = error < 0 ? -error : error; fatal(what); }
}

/* Encoding mirrors libxfs xfs_ialloc.c's private xfs_inobt_update. */
static int update_record(struct xfs_btree_cur *cur,
                         const struct xfs_inobt_rec_incore *irec)
{
    union xfs_btree_rec rec = {0};
    rec.inobt.ir_startino = cpu_to_be32(irec->ir_startino);
    if (xfs_has_sparseinodes(cur->bc_mp)) {
        rec.inobt.ir_u.sp.ir_holemask = cpu_to_be16(irec->ir_holemask);
        rec.inobt.ir_u.sp.ir_count = irec->ir_count;
        rec.inobt.ir_u.sp.ir_freecount = irec->ir_freecount;
    } else {
        rec.inobt.ir_u.f.ir_freecount = cpu_to_be32(irec->ir_freecount);
    }
    rec.inobt.ir_free = cpu_to_be64(irec->ir_free);
    return libxfs_btree_update(cur, &rec);
}

xfs_ino_t inode_chunk_start(struct xfs_mount *mp, xfs_ino_t ino)
{
    struct xfs_perag *pag = libxfs_perag_get(mp, XFS_INO_TO_AGNO(mp, ino));
    struct xfs_buf *bp;
    struct xfs_btree_cur *cur;
    struct xfs_inobt_rec_incore rec;
    xfs_agino_t agino = XFS_INO_TO_AGINO(mp, ino);
    xfs_ino_t first;
    int found;
    if (!pag) { errno = EFSCORRUPTED; fatal("missing inode allocation group"); }
    need(libxfs_ialloc_read_agi(pag, NULL, 0, &bp), "read inode allocation group");
    cur = libxfs_inobt_init_cursor(pag, NULL, bp);
    need(xfs_inobt_lookup(cur, agino, XFS_LOOKUP_LE, &found), "find inode chunk");
    if (!found) { errno = EFSCORRUPTED; fatal("missing inode chunk"); }
    need(xfs_inobt_get_rec(cur, &rec, &found), "read inode chunk");
    if (!found || agino < rec.ir_startino || agino - rec.ir_startino >= 64) {
        errno = EFSCORRUPTED; fatal("inode outside allocation chunk");
    }
    first = XFS_AGINO_TO_INO(mp, XFS_INO_TO_AGNO(mp, ino), rec.ir_startino);
    libxfs_btree_del_cursor(cur, XFS_BTREE_NOERROR);
    libxfs_buf_relse(bp);
    libxfs_perag_put(pag);
    return first;
}

/* Offline equivalent of repeated xfs_difree calls for one 64-slot record.
 * Inode chunks remain allocated, so the two allocation indexes retain the
 * same record and no cached inode cluster can become stale. Both redundant
 * index records are checked BEFORE either is updated. */
void free_inode_mask(struct xfs_trans *tp, xfs_ino_t first, uint64_t mask)
{
    struct xfs_mount *mp = tp->t_mountp;
    struct xfs_perag *pag;
    struct xfs_buf *bp;
    struct xfs_agi *agi;
    struct xfs_btree_cur *icur, *fcur = NULL;
    struct xfs_inobt_rec_incore old, freerec, updated;
    xfs_agino_t start = XFS_INO_TO_AGINO(mp, first);
    unsigned count = 0;
    uint64_t bits;
    int found, ffound = 0;
    if (!mask) return;
    for (bits = mask; bits; bits &= bits - 1) ++count;
    pag = libxfs_perag_get(mp, XFS_INO_TO_AGNO(mp, first));
    if (!pag) { errno = EFSCORRUPTED; fatal("missing allocation group"); }
    need(libxfs_ialloc_read_agi(pag, tp, 0, &bp), "read inode allocation header");
    agi = bp->b_addr;
    icur = libxfs_inobt_init_cursor(pag, tp, bp);
    need(xfs_inobt_lookup(icur, start, XFS_LOOKUP_EQ, &found), "lookup inode allocation record");
    if (!found) { errno = EFSCORRUPTED; fatal("missing inode allocation record"); }
    need(xfs_inobt_get_rec(icur, &old, &found), "read inode allocation record");
    if (!found || old.ir_startino != start || (old.ir_free & mask) ||
        (xfs_inobt_irec_to_allocmask(&old) & mask) != mask ||
        old.ir_freecount + count > old.ir_count ||
        be32_to_cpu(agi->agi_freecount) != pag->pagi_freecount) {
        errno = EFSCORRUPTED; fatal("invalid batch of inode frees");
    }
    if (xfs_has_finobt(mp)) {
        fcur = libxfs_finobt_init_cursor(pag, tp, bp);
        need(xfs_inobt_lookup(fcur, start, XFS_LOOKUP_EQ, &ffound), "lookup free-inode record");
        if (ffound) {
            need(xfs_inobt_get_rec(fcur, &freerec, &found), "read free-inode record");
            if (!found || !old.ir_freecount || freerec.ir_startino != old.ir_startino ||
                freerec.ir_holemask != old.ir_holemask || freerec.ir_count != old.ir_count ||
                freerec.ir_freecount != old.ir_freecount || freerec.ir_free != old.ir_free) {
                errno = EFSCORRUPTED; fatal("inode indexes disagree");
            }
        } else if (old.ir_freecount) {
            errno = EFSCORRUPTED; fatal("missing redundant free-inode record");
        }
    }
    updated = old;
    updated.ir_free |= mask;
    updated.ir_freecount += count;
    need(update_record(icur, &updated), "update inode allocation bitmap");
    if (fcur) {
        if (ffound) {
            freerec.ir_free |= mask;
            freerec.ir_freecount += count;
            need(update_record(fcur, &freerec), "update free-inode bitmap");
        } else {
            need(xfs_inobt_insert_rec(fcur, updated.ir_holemask, updated.ir_count,
                updated.ir_freecount, updated.ir_free, &found), "insert free-inode record");
            if (!found) { errno = EFSCORRUPTED; fatal("free-inode insertion failed"); }
        }
        libxfs_btree_del_cursor(fcur, XFS_BTREE_NOERROR);
    }
    libxfs_btree_del_cursor(icur, XFS_BTREE_NOERROR);
    be32_add_cpu(&agi->agi_freecount, count);
    xfs_ialloc_log_agi(tp, bp, XFS_AGI_FREECOUNT);
    pag->pagi_freecount += count;
    xfs_trans_mod_sb(tp, XFS_TRANS_SB_IFREE, count);
    libxfs_perag_put(pag);
}
